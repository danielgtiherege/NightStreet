/*
 * NIGHT STREET -- cidade noturna com modelos OBJ/MTL, um unico arquivo C.
 * Windows, GLFW, OpenGL 2.1 classico, 1024x768. Sem GLEW/GLU/Assimp.
 *
 * PowerShell / MinGW:
 * gcc mainTex.c -std=c99 -O2 -Iinclude -Llib -lglfw3dll -lopengl32 -lgdi32 -lm -o demo3D.exe
 *
 * Ao lado do .exe: glfw3.dll, floor.jpg e a pasta MATERIAIS.
 *
 * ESC sair | ESPACO pausar | R reiniciar percurso | V VSync
 * CIMA/BAIXO velocidade | F aramado | C culling
 * ALT+ENTER ou duplo clique esquerdo: tela cheia/janela. Usa o monitor em
 * que a janela esta e sua resolucao/refresh atuais. Restaura tamanho/posicao
 * da janela ao sair. HUD no canto superior direito somente em tela cheia.
 * CITY_SEED=0 sorteia a cidade a cada inicio; outro inteiro fixa a geracao.
 * SHIP_ALTITUDE e a altura da BASE do casco. O OBJ do navio e Z-up e recebe
 * conversao automatica para Y-up, centralizacao e escala SHIP_LENGTH.
 * Um navio, rota independente em sentido contrario, com leve flutuacao.
 * O navio ignora o culling manual. A camera antecipa os encontros, sobe
 * acima dos mastros e acompanha o navio ate o casco inteiro passar.
 * CAMERA_SHIP_* controla antecipacao, altura, subida, espera e retorno.
 *
 * OBJ/MTL e imagens sao carregados UMA vez. Modelos, postes, lampadas e chao
 * usam display lists reutilizadas; AABBs descartam instancias fora da visao.
 * As oito luzes sao escolhidas somente entre lampadas no campo de visao.
 * Mantem 20% de lampadas defeituosas. Nao implementa sombras/oclusao/PBR.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#ifndef GLFW_DLL
#define GLFW_DLL
#endif
#endif
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <wchar.h>
#include <time.h>

#define PI 3.14159265358979323846
#define CAMERA_SPEED 5.0
#define CAMERA_HEIGHT 1.75f
#define FOG_DENSITY 0.025f
#define LIGHT_POWER 1.45f
#define USE_TEXTURES 1
#define CITY_SEED 0u /* 0 = aleatorio; outro valor = cidade reproduzivel */
#define SHIP_SPEED 3.25
#define SHIP_LENGTH 17.5f
#define SHIP_ALTITUDE 10.5f
#define SHIP_YAW_OFFSET 0.0f
#define SHIP_LANE_OFFSET 3.0f
#define CAMERA_SHIP_LOOKAHEAD 12.0
#define CAMERA_SHIP_ASCENT_TIME 5.0
#define CAMERA_SHIP_RETURN_TIME 4.0
#define CAMERA_SHIP_HOLD_AFTER_PASS 2.5
#define CAMERA_SHIP_CLEARANCE -5.5f
#define CAMERA_SHIP_ENCOUNTER_RADIUS 8.0f
#define GROUND_CHUNK 12.0f
#define MAX_PARTS 512
#define LAMP_HEIGHT 8.5f
#define FLICKER_PERCENT 20
#define FLOOR_REPEAT 2.5f
#define WALL_REPEAT 3.0f
#define MAX_LAMPS 512
#define LIGHT_SLOTS 8
#define CITY_EDGE 88.0f
#define ROAD_HALF 6.5f
#define TILE 1.5f

typedef struct { float x, y, z; } Vec3;
typedef struct {
    Vec3 bulb;
    int faulty;
    unsigned random;
    double nextChange;
    float intensity;
    GLuint bodyList;
    Vec3 min,max;
    int visible;
} Lamp;
typedef struct {
    GLuint list;
    Vec3 min,max;
} RenderPart;
static Lamp lamps[MAX_LAMPS];
static int lampCount, paused, wireframe, vsync = 1;
static int culling=1,partCount,visibleParts,visibleLamps;
static RenderPart parts[MAX_PARTS];
static GLfloat frustum[6][4];
static double travel, animationTime, speed = CAMERA_SPEED;
static GLuint bulbList, floorTexture;
static const float roadsX[] = {-64,-30,12,64};
static const float roadsZ[] = {-60,-22,20,60};

static float minimum(float a, float b) { return a < b ? a : b; }
static Vec3 vec(float x, float y, float z) { Vec3 v = {x,y,z}; return v; }
static Vec3 sub(Vec3 a, Vec3 b) { return vec(a.x-b.x,a.y-b.y,a.z-b.z); }
static float dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3 cross(Vec3 a, Vec3 b)
{ return vec(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
static Vec3 unit(Vec3 v)
{
    float length = sqrtf(dot(v,v));
    if (length < 0.00001f) return vec(0,0,-1);
    return vec(v.x/length,v.y/length,v.z/length);
}

/* Matriz de camera equivalente a lookAt, sem depender de gluLookAt. */
static void camera(Vec3 eye, Vec3 target)
{
    Vec3 f = unit(sub(target,eye));
    Vec3 s = unit(cross(f,vec(0,1,0)));
    Vec3 u = cross(s,f);
    GLfloat m[16] = {s.x,u.x,-f.x,0, s.y,u.y,-f.y,0,
                     s.z,u.z,-f.z,0, 0,0,0,1};
    glLoadIdentity();
    glMultMatrixf(m);
    glTranslatef(-eye.x,-eye.y,-eye.z);
}

/* Percurso fechado pela malha de ruas, incluindo vias internas.
 * Retas e curvas de 90 graus sao parametrizadas por distancia real. */
#define MAX_PATH_SEGMENTS 32
typedef struct { Vec3 a,b,center; double length,angle,sweep; int arc; } PathSegment;
static PathSegment pathSegments[MAX_PATH_SEGMENTS];
static int pathCount;
static double pathLength;
static void buildPath(void)
{
    Vec3 corners[]={ {-64,0,-60},{64,0,-60},{64,0,60},{-64,0,60},
                     {-64,0,-22},{12,0,-22},{12,0,20},{-30,0,20},{-30,0,-22},{-64,0,-22} };
    Vec3 before[10],after[10],centers[10];
    double sweep[10]; int i;
    for(i=0;i<10;++i) {
        Vec3 p=corners[i],incoming=unit(sub(p,corners[(i+9)%10]));
        Vec3 outgoing=unit(sub(corners[(i+1)%10],p));
        float r=7.0f;
        before[i]=vec(p.x-incoming.x*r,0,p.z-incoming.z*r);
        after[i]=vec(p.x+outgoing.x*r,0,p.z+outgoing.z*r);
        centers[i]=vec(before[i].x+outgoing.x*r,0,before[i].z+outgoing.z*r);
        sweep[i]=(incoming.x*outgoing.z-incoming.z*outgoing.x)>0?PI/2:-PI/2;
    }
    pathCount=0; pathLength=0;
    for(i=0;i<10;++i) {
        PathSegment *line=&pathSegments[pathCount++],*arc=&pathSegments[pathCount++];
        Vec3 delta;
        line->a=after[(i+9)%10]; line->b=before[i]; delta=sub(line->b,line->a);
        line->length=sqrt(dot(delta,delta)); line->arc=0;
        arc->a=before[i]; arc->b=after[i]; arc->center=centers[i]; arc->arc=1;
        arc->angle=atan2(arc->a.z-arc->center.z,arc->a.x-arc->center.x);
        arc->sweep=sweep[i]; arc->length=7*PI/2;
        pathLength+=line->length+arc->length;
    }
}
static Vec3 route(double distance)
{
    int i; double d=fmod(distance,pathLength);
    if(d<0) d+=pathLength;
    for(i=0;i<pathCount;++i) {
        PathSegment *s=&pathSegments[i];
        if(d<=s->length || i==pathCount-1) {
            double t=d/s->length;
            if(s->arc) {
                double a=s->angle+s->sweep*t;
                return vec(s->center.x+7*(float)cos(a),CAMERA_HEIGHT,s->center.z+7*(float)sin(a));
            }
            return vec(s->a.x+(s->b.x-s->a.x)*(float)t,CAMERA_HEIGHT,s->a.z+(s->b.z-s->a.z)*(float)t);
        }
        d-=s->length;
    }
    return vec(0,CAMERA_HEIGHT,0);
}

static Vec3 shipPosition(double time)
{
    double distance=132.0-time*SHIP_SPEED;
    Vec3 p=route(distance),direction=unit(sub(route(distance-.5),p));
    p.x+=-direction.z*SHIP_LANE_OFFSET; p.z+=direction.x*SHIP_LANE_OFFSET;
    p.y=SHIP_ALTITUDE+.40f*(float)sin(time*.8);
    return p;
}

/* A passagem usa distancia prevista, nao um ciclo fixo de 28 segundos.
 * A camera sobe ANTES do encontro, acompanha o centro do navio inclusive
 * quando ele fica para tras, e so entao desce/volta a olhar para a rua.
 * O teste de encontro considera trajetorias futuras e a velocidade atual,
 * funcionando tambem ao mudar a velocidade, pausar ou reiniciar com R. */
static int shipCameraMode; /* 0: passeio; 1: acompanhar; 2: retornar */
static double shipCameraStart,shipCameraReturn,shipCameraClosestTime;
static float shipCameraClosestDistance,shipCameraBlend;
static int shipCameraAvailable;
static float shipCameraModelHeight=SHIP_LENGTH*.82f;

static float smoothStep01(float x)
{
    x=fminf(1,fmaxf(0,x));
    return x*x*(3-2*x);
}

static void resetShipCamera(void)
{
    shipCameraMode=0; shipCameraBlend=0;
    shipCameraClosestDistance=1e30f;
    shipCameraStart=shipCameraReturn=shipCameraClosestTime=animationTime;
}

static int shipEncounterAhead(void)
{
    float closest=1e30f; double t,closestAt=0;
    /* Antecedencia suficiente para terminar a subida antes da proa chegar.
     * Usa apenas a distancia horizontal: o navio esta sempre no ar. */
    for(t=0;t<=CAMERA_SHIP_LOOKAHEAD;t+=.25) {
        Vec3 c=route(travel+speed*t),s=shipPosition(animationTime+t);
        float dx=s.x-c.x,dz=s.z-c.z,distance=dx*dx+dz*dz;
        if(distance<closest) { closest=distance; closestAt=t; }
    }
    return closest<CAMERA_SHIP_ENCOUNTER_RADIUS*CAMERA_SHIP_ENCOUNTER_RADIUS && closestAt>.75;
}

static void animateCamera(Vec3 *eye,Vec3 *target)
{
    Vec3 ship=shipPosition(animationTime),forward=unit(sub(route(travel+.5),route(travel)));
    Vec3 right=vec(-forward.z,0,forward.x);
    float dx=ship.x-eye->x,dz=ship.z-eye->z;
    float horizontalDistance=sqrtf(dx*dx+dz*dz),blend;
    if(!shipCameraAvailable) return;
    if(shipCameraMode==0 && shipEncounterAhead()) {
        shipCameraMode=1; shipCameraStart=animationTime;
        shipCameraClosestTime=animationTime;
        shipCameraClosestDistance=horizontalDistance;
    }
    if(shipCameraMode==1) {
        if(horizontalDistance<shipCameraClosestDistance) {
            shipCameraClosestDistance=horizontalDistance;
            shipCameraClosestTime=animationTime;
        }
        /* Espera o casco inteiro passar e segura o olhar ainda por um tempo.
         * Nao abandona o alvo so porque o navio saiu da frente da camera. */
        if((shipCameraClosestDistance<CAMERA_SHIP_ENCOUNTER_RADIUS &&
            animationTime-shipCameraClosestTime>CAMERA_SHIP_HOLD_AFTER_PASS &&
            horizontalDistance>SHIP_LENGTH*.5f+12.0f) ||
            animationTime-shipCameraStart>45.0) {
            shipCameraMode=2; shipCameraReturn=animationTime;
        }
    }
    if(shipCameraMode==1)
        blend=smoothStep01((float)((animationTime-shipCameraStart)/CAMERA_SHIP_ASCENT_TIME));
    else if(shipCameraMode==2) {
        blend=1-smoothStep01((float)((animationTime-shipCameraReturn)/CAMERA_SHIP_RETURN_TIME));
        if(blend<=0) shipCameraMode=0;
    } else blend=0;
    shipCameraBlend=blend;
    /* Fica acima dos mastros, com distancia para enquadrar proa e popa.
     * Mantem a posicao horizontal sobre a rua, afastada da faixa do navio. */
    eye->y=CAMERA_HEIGHT+(SHIP_ALTITUDE+shipCameraModelHeight+CAMERA_SHIP_CLEARANCE-CAMERA_HEIGHT)*blend;
    eye->x+=right.x*2.0f*blend; eye->z+=right.z*2.0f*blend;
    ship.y+=shipCameraModelHeight*.5f;
    target->x+=(ship.x-target->x)*blend;
    target->y+=(ship.y-target->y)*blend;
    target->z+=(ship.z-target->z)*blend;
}

/* JPG em um unico C: usa dinamicamente o decodificador GDI+ do Windows.
 * As estruturas abaixo reproduzem a ABI C; nao requer gdiplus.h/C++.
 * Copia RGB com o passo de linha retornado e inverte Y para o OpenGL. */
#ifdef _WIN32
typedef struct {
    UINT version;
    void (WINAPI *debugCallback)(int,const char *);
    BOOL noThread, noExternalCodecs;
} DemoGdipInput;
typedef struct { INT x,y,width,height; } DemoGdipRect;
typedef struct {
    UINT width,height;
    INT stride,format;
    void *scan0;
    UINT_PTR reserved;
} DemoGdipData;

static GLuint loadTexture(const wchar_t *filename)
{
    typedef int (WINAPI *StartupFn)(ULONG_PTR *,const DemoGdipInput *,void *);
    typedef void (WINAPI *ShutdownFn)(ULONG_PTR);
    typedef int (WINAPI *CreateFn)(const wchar_t *,void **);
    typedef int (WINAPI *SizeFn)(void *,UINT *);
    typedef int (WINAPI *LockFn)(void *,const DemoGdipRect *,UINT,INT,DemoGdipData *);
    typedef int (WINAPI *UnlockFn)(void *,DemoGdipData *);
    typedef int (WINAPI *DisposeFn)(void *);
    StartupFn startup=NULL; ShutdownFn shutdown=NULL; CreateFn create=NULL;
    SizeFn getWidth=NULL,getHeight=NULL; LockFn lock=NULL;
    UnlockFn unlock=NULL; DisposeFn dispose=NULL;
    HMODULE module=LoadLibraryW(L"gdiplus.dll");
    ULONG_PTR token=0;
    DemoGdipInput input={1,NULL,FALSE,FALSE};
    DemoGdipData data={0}; DemoGdipRect rect;
    void *bitmap=NULL; unsigned char *pixels=NULL;
    UINT width=0,height=0,x,y; GLuint texture=0;
    GLint maximum=0; int locked=0,started=0;
    wchar_t fullPath[32768];
    if(!module) return 0;
#define LOAD_GDIP(variable,symbol) do { \
    FARPROC proc=GetProcAddress(module,symbol); \
    if(!proc) goto done; \
    memcpy(&variable,&proc,sizeof(variable)); \
} while(0)
    LOAD_GDIP(startup,"GdiplusStartup");
    LOAD_GDIP(shutdown,"GdiplusShutdown");
    LOAD_GDIP(create,"GdipCreateBitmapFromFile");
    LOAD_GDIP(getWidth,"GdipGetImageWidth");
    LOAD_GDIP(getHeight,"GdipGetImageHeight");
    LOAD_GDIP(lock,"GdipBitmapLockBits");
    LOAD_GDIP(unlock,"GdipBitmapUnlockBits");
    LOAD_GDIP(dispose,"GdipDisposeImage");
#undef LOAD_GDIP
    if(startup(&token,&input,NULL)!=0) goto done;
    started=1;
    /* Primeiro procura ao lado do .exe, depois no diretorio atual. */
    { DWORD count=GetModuleFileNameW(NULL,fullPath,32768);
      if(count>0 && count<32768) {
          wchar_t *slash=wcsrchr(fullPath,L'\\');
          if(slash && (size_t)(slash-fullPath)+1+wcslen(filename)<32768) {
              wcscpy(slash+1,filename);
              if(create(fullPath,&bitmap)!=0) bitmap=NULL;
          }
      }
    }
    if(!bitmap && create(filename,&bitmap)!=0) goto done;
    if(getWidth(bitmap,&width)!=0 || getHeight(bitmap,&height)!=0) goto done;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maximum);
    if(!width || !height || width>(UINT)maximum || height>(UINT)maximum) goto done;
    if((size_t)height > (size_t)-1/3/width) goto done;
    rect.x=rect.y=0; rect.width=(INT)width; rect.height=(INT)height;
    /* ImageLockModeRead = 1, PixelFormat32bppARGB = 0x0026200A. */
    if(lock(bitmap,&rect,1,0x0026200A,&data)!=0) goto done;
    locked=1;
    pixels=(unsigned char *)malloc((size_t)width*height*3);
    if(!pixels) goto done;
    for(y=0;y<height;++y) {
        const unsigned char *row=(const unsigned char *)data.scan0+
                                  (ptrdiff_t)(height-1-y)*data.stride;
        unsigned char *out=pixels+(size_t)y*width*3;
        for(x=0;x<width;++x) {
            out[x*3]=row[x*4+2]; out[x*3+1]=row[x*4+1]; out[x*3+2]=row[x*4];
        }
    }
    glGenTextures(1,&texture);
    glBindTexture(GL_TEXTURE_2D,texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,(GLsizei)width,(GLsizei)height,0,
                 GL_RGB,GL_UNSIGNED_BYTE,pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    glBindTexture(GL_TEXTURE_2D,0);
done:
    free(pixels);
    if(locked) unlock(bitmap,&data);
    if(bitmap) dispose(bitmap);
    if(started) shutdown(token);
    FreeLibrary(module);
    if(!texture) fprintf(stderr,"Nao foi possivel carregar %ls; usando cor.\n",filename);
    return texture;
}
#else
static GLuint loadTexture(const wchar_t *filename)
{ (void)filename; return 0; } /* Esta demo tem Windows como alvo. */
#endif

/* OBJ/MTL: indices positivos/negativos, UVs, normais, cores Kd e map_Kd.
 * Poligonos concavos sao triangulados por ear clipping na carga.
 * Dados de CPU sao liberados depois da compilacao da display list.
 * Este importador simples nao implementa PBR, transparencia ou animacao OBJ.
 */
#define MAX_MODELS 64
#define MAX_MATERIALS 128
#define MAX_TEXTURES 128
#define MAX_INSTANCES 1024
#define PATH_SIZE 1024

typedef struct { char name[128]; float kd[3]; GLuint texture; } ObjMaterial;
typedef struct { int v,t,n; } ObjIndex;
typedef struct { ObjIndex p[3]; int material; } ObjTriangle;
typedef struct { float u,v; } ObjUV;
typedef struct {
    char name[96]; GLuint list;
    Vec3 min,max; int triangles;
} Model;
typedef struct {
    int model; Vec3 position,min,max;
    float scale,yaw;
} Instance;
typedef struct { char path[PATH_SIZE]; GLuint texture; } TextureEntry;
static Model models[MAX_MODELS];
static Instance instances[MAX_INSTANCES];
static TextureEntry textures[MAX_TEXTURES];
static int modelCount,instanceCount,textureCount,buildingModels[24],buildingCount;
static int carModels[12],carCount,signModels[3],trafficModel=-1,coneModel=-1;
static int shipModel=-1,buildingInstances,carInstances,propInstances,visibleInstances;
static char assetRoot[PATH_SIZE];
static unsigned cityRandom=CITY_SEED;
static int importFailed;

static void *growArray(void *ptr,size_t *capacity,size_t needed,size_t size)
{
    void *result; size_t next;
    if(needed<=*capacity) return ptr;
    next=*capacity?*capacity*2:256;
    if(next<needed) next=needed;
    if(next>(size_t)-1/size) return NULL;
    result=realloc(ptr,next*size);
    if(result) *capacity=next;
    return result;
}

static void joinPath(char *out,size_t capacity,const char *parent,const char *name)
{
    size_t n;
    if((name[0] && name[1]==':') || name[0]=='/' || name[0]=='\\')
        snprintf(out,capacity,"%s",name);
    else snprintf(out,capacity,"%s%s%s",parent,*parent?"/":"",name);
    for(n=0;out[n];++n) if(out[n]=='\\') out[n]='/';
}

static void parentPath(char *out,size_t capacity,const char *path)
{
    char *slash;
    snprintf(out,capacity,"%s",path);
    slash=strrchr(out,'/');
    if(slash) *slash=0; else *out=0;
}

static int existsFile(const char *path)
{
    FILE *f=fopen(path,"rb");
    if(!f) return 0;
    fclose(f); return 1;
}

static void locateAssets(void)
{
    const char *roots[]={"MATERIAIS","materiais"};
    char path[PATH_SIZE]; int i;
    snprintf(assetRoot,sizeof(assetRoot),"MATERIAIS");
    for(i=0;i<4;++i) {
        joinPath(path,sizeof(path),roots[i],"predios/building-a.obj");
        if(existsFile(path)) { snprintf(assetRoot,sizeof(assetRoot),"%s",roots[i]); break; }
    }
    printf("Pasta dos modelos: %s\n",assetRoot);
}

static GLuint textureFromFile(const char *path)
{
    wchar_t wide[PATH_SIZE]; GLuint id; int i;
    if(!USE_TEXTURES || !*path) return 0;
    for(i=0;i<textureCount;++i) if(strcmp(textures[i].path,path)==0)
        return textures[i].texture;
    if(textureCount>=MAX_TEXTURES) { importFailed=1; return 0; }
#ifdef _WIN32
    if(!MultiByteToWideChar(CP_UTF8,0,path,-1,wide,PATH_SIZE)) return 0;
#else
    if(mbstowcs(wide,path,PATH_SIZE)==(size_t)-1) return 0;
#endif
    id=loadTexture(wide);
    snprintf(textures[textureCount].path,PATH_SIZE,"%s",path);
    textures[textureCount++].texture=id;
    return id;
}

static int materialId(ObjMaterial *materials,int *count,const char *name)
{
    int i;
    for(i=0;i<*count;++i) if(strcmp(materials[i].name,name)==0) return i;
    if(*count>=MAX_MATERIALS) return 0;
    i=(*count)++;
    snprintf(materials[i].name,sizeof(materials[i].name),"%s",name);
    materials[i].kd[0]=materials[i].kd[1]=materials[i].kd[2]=.65f;
    materials[i].texture=0; return i;
}

static char *trim(char *s)
{
    char *end;
    while(*s==' ' || *s=='\t') ++s;
    end=s+strlen(s);
    while(end>s && (end[-1]=='\n'||end[-1]=='\r'||end[-1]==' '||end[-1]=='\t')) --end;
    *end=0; return s;
}

static void loadMaterials(const char *path,ObjMaterial *materials,int *count)
{
    FILE *f=fopen(path,"r"); char line[4096],directory[PATH_SIZE]; int current=0;
    if(!f) { fprintf(stderr,"MTL ausente: %s; usando cores simples.\n",path); return; }
    parentPath(directory,sizeof(directory),path);
    while(fgets(line,sizeof(line),f)) {
        char *s=trim(line);
        if(strncmp(s,"newmtl ",7)==0) current=materialId(materials,count,trim(s+7));
        else if(strncmp(s,"Kd ",3)==0)
            sscanf(s+3,"%f %f %f",&materials[current].kd[0],&materials[current].kd[1],&materials[current].kd[2]);
        else if(strncmp(s,"map_Kd ",7)==0) {
            char pathTexture[PATH_SIZE],*name=trim(s+7); size_t len=strlen(name);
            if(len>1 && name[0]=='"' && name[len-1]=='"') { name[len-1]=0; ++name; }
            if(name[0]=='-') { fprintf(stderr,"map_Kd com opcoes nao suportado: %s\n",name); continue; }
            joinPath(pathTexture,sizeof(pathTexture),directory,name);
            materials[current].texture=textureFromFile(pathTexture);
        }
    }
    fclose(f);
}

static int objIndex(int index,size_t count)
{
    if(index>0 && (size_t)index<=count) return index-1;
    if(index<0 && (size_t)(-index)<=count) return (int)count+index;
    return -1;
}

static ObjIndex readObjIndex(char *word,size_t nv,size_t nt,size_t nn)
{
    ObjIndex r={-1,-1,-1}; char *slash=strchr(word,'/');
    r.v=objIndex(atoi(word),nv);
    if(slash) {
        r.t=objIndex(atoi(slash+1),nt);
        slash=strchr(slash+1,'/');
        if(slash) r.n=objIndex(atoi(slash+1),nn);
    }
    return r;
}

static float area2(float ax,float ay,float bx,float by,float cx,float cy)
{ return (bx-ax)*(cy-ay)-(by-ay)*(cx-ax); }

static int addTriangle(ObjTriangle **tris,size_t *count,size_t *capacity,
                       ObjIndex a,ObjIndex b,ObjIndex c,int material)
{
    ObjTriangle *next=growArray(*tris,capacity,*count+1,sizeof(**tris));
    if(!next) return 0;
    *tris=next;
    next[*count].p[0]=a; next[*count].p[1]=b; next[*count].p[2]=c;
    next[*count].material=material; ++*count; return 1;
}

static int triangulate(ObjIndex *face,int n,Vec3 *v,int material,
                       ObjTriangle **tri,size_t *count,size_t *capacity)
{
    float x[128],y[128],signedArea=0,sign;
    Vec3 normal=vec(0,0,0); int order[128],i,j,left=n,guard=0,axis;
    if(n==3) return addTriangle(tri,count,capacity,face[0],face[1],face[2],material);
    for(i=0;i<n;++i) {
        Vec3 a=v[face[i].v],b=v[face[(i+1)%n].v];
        normal.x+=(a.y-b.y)*(a.z+b.z); normal.y+=(a.z-b.z)*(a.x+b.x);
        normal.z+=(a.x-b.x)*(a.y+b.y);
    }
    axis=fabsf(normal.x)>fabsf(normal.y)?0:1;
    if(fabsf(normal.z)>(axis==0?fabsf(normal.x):fabsf(normal.y))) axis=2;
    for(i=0;i<n;++i) {
        Vec3 p=v[face[i].v]; order[i]=i;
        x[i]=axis==0?p.y:p.x; y[i]=axis==2?p.y:p.z;
    }
    for(i=0;i<n;++i) signedArea+=x[i]*y[(i+1)%n]-x[(i+1)%n]*y[i];
    sign=signedArea<0?-1.0f:1.0f;
    while(left>3 && ++guard<n*n) {
        int found=0;
        for(i=0;i<left;++i) {
            int a=order[(i+left-1)%left],b=order[i],c=order[(i+1)%left],inside=0;
            if(sign*area2(x[a],y[a],x[b],y[b],x[c],y[c])<=1e-10f) continue;
            for(j=0;j<left;++j) {
                int p=order[j];
                if(p==a||p==b||p==c) continue;
                if(sign*area2(x[a],y[a],x[b],y[b],x[p],y[p])>1e-9f &&
                   sign*area2(x[b],y[b],x[c],y[c],x[p],y[p])>1e-9f &&
                   sign*area2(x[c],y[c],x[a],y[a],x[p],y[p])>1e-9f) { inside=1; break; }
            }
            if(inside) continue;
            if(!addTriangle(tri,count,capacity,face[a],face[b],face[c],material)) return 0;
            memmove(&order[i],&order[i+1],(size_t)(left-i-1)*sizeof(order[0]));
            --left; found=1; break;
        }
        if(!found) break;
    }
    /* Poligonos degenerados/nao planares: fan somente no resto nao resolvido. */
    for(i=1;i<left-1;++i)
        if(!addTriangle(tri,count,capacity,face[order[0]],face[order[i]],face[order[i+1]],material)) return 0;
    return 1;
}

static Vec3 shipAxis(Vec3 p) { return vec(p.x,p.z,-p.y); }

static int loadObj(const char *path,int zUp)
{
    FILE *f=fopen(path,"r");
    Vec3 *vertices=NULL,*normals=NULL,min=vec(1e30f,1e30f,1e30f),max=vec(-1e30f,-1e30f,-1e30f),offset;
    ObjUV *uv=NULL; ObjTriangle *tri=NULL;
    ObjMaterial materials[MAX_MATERIALS];
    size_t nv=0,nn=0,nt=0,ntri=0,cv=0,cn=0,ct=0,cf=0,i;
    char line[16384],directory[PATH_SIZE];
    int material=0,materialCount=0,result=-1,failed=0;
    GLuint list=0;
    if(!f || modelCount>=MAX_MODELS) { if(f) fclose(f); fprintf(stderr,"OBJ indisponivel: %s\n",path); return -1; }
    materialId(materials,&materialCount,"default");
    parentPath(directory,sizeof(directory),path);
    while(fgets(line,sizeof(line),f)) {
        char *s=trim(line);
        if(s[0]=='v' && s[1]==' ') {
            Vec3 p; Vec3 *next;
            if(sscanf(s+2,"%f %f %f",&p.x,&p.y,&p.z)!=3) continue;
            if(zUp) p=shipAxis(p);
            next=growArray(vertices,&cv,nv+1,sizeof(*vertices));
            if(!next) { failed=1; break; } vertices=next; vertices[nv++]=p;
            min=vec(fminf(min.x,p.x),fminf(min.y,p.y),fminf(min.z,p.z));
            max=vec(fmaxf(max.x,p.x),fmaxf(max.y,p.y),fmaxf(max.z,p.z));
        } else if(strncmp(s,"vn ",3)==0) {
            Vec3 p,*next;
            if(sscanf(s+3,"%f %f %f",&p.x,&p.y,&p.z)!=3) continue;
            if(zUp) p=shipAxis(p);
            next=growArray(normals,&cn,nn+1,sizeof(*normals));
            if(!next) { failed=1; break; } normals=next; normals[nn++]=unit(p);
        } else if(strncmp(s,"vt ",3)==0) {
            ObjUV p,*next;
            if(sscanf(s+3,"%f %f",&p.u,&p.v)!=2) continue;
            next=growArray(uv,&ct,nt+1,sizeof(*uv));
            if(!next) { failed=1; break; } uv=next; uv[nt++]=p;
        } else if(strncmp(s,"mtllib ",7)==0) {
            char mtlPath[PATH_SIZE]; joinPath(mtlPath,sizeof(mtlPath),directory,trim(s+7));
            loadMaterials(mtlPath,materials,&materialCount);
        } else if(strncmp(s,"usemtl ",7)==0) material=materialId(materials,&materialCount,trim(s+7));
        else if(s[0]=='f' && s[1]==' ') {
            ObjIndex face[128]; char *token=strtok(s+2," \t\r\n"); int n=0,valid=1;
            while(token) {
                if(n>=128) { valid=0; break; }
                face[n]=readObjIndex(token,nv,nt,nn);
                if(face[n].v<0) valid=0;
                ++n; token=strtok(NULL," \t\r\n");
            }
            if(valid && n>=3 && !triangulate(face,n,vertices,material,&tri,&ntri,&cf)) { failed=1; break; }
        }
    }
    fclose(f);
    if(failed || !ntri || !nv) goto cleanup;
    offset=vec((min.x+max.x)*.5f,min.y,(min.z+max.z)*.5f);
    list=glGenLists(1);
    if(!list) goto cleanup;
    glNewList(list,GL_COMPILE);
    { int current=-1;
      for(i=0;i<ntri;++i) {
        ObjTriangle *t=&tri[i]; Vec3 normal;
        int j;
        if(t->material!=current) {
            ObjMaterial *m=&materials[t->material];
            if(current>=0) glEnd();
            if(m->texture) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,m->texture); }
            else glDisable(GL_TEXTURE_2D);
            glColor3fv(m->kd); current=t->material; glBegin(GL_TRIANGLES);
        }
        normal=unit(cross(sub(vertices[t->p[1].v],vertices[t->p[0].v]),sub(vertices[t->p[2].v],vertices[t->p[0].v])));
        for(j=0;j<3;++j) {
            ObjIndex p=t->p[j]; Vec3 n=p.n>=0?normals[p.n]:normal,position=sub(vertices[p.v],offset);
            glNormal3f(n.x,n.y,n.z);
            if(p.t>=0) glTexCoord2f(uv[p.t].u,uv[p.t].v); else glTexCoord2f(0,0);
            glVertex3f(position.x,position.y,position.z);
        }
      }
      glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glEndList();
    result=modelCount++;
    snprintf(models[result].name,sizeof(models[result].name),"%.95s",strrchr(path,'/')?strrchr(path,'/')+1:path);
    models[result].list=list; models[result].min=sub(min,offset); models[result].max=sub(max,offset);
    models[result].triangles=(int)ntri;
    printf("Modelo: %-30s %d triangulos\n",models[result].name,(int)ntri);
cleanup:
    if(result<0) { fprintf(stderr,"Falha ao importar: %s\n",path); importFailed=1; }
    free(vertices); free(normals); free(uv); free(tri);
    return result;
}

static int modelFromAssets(const char *name,int ship)
{
    char path[PATH_SIZE],relative[PATH_SIZE];
    snprintf(relative,sizeof(relative),"%s/%s",ship?"navio":"predios",name);
    joinPath(path,sizeof(path),assetRoot,relative);
    if(ship && !existsFile(path)) {
        const char *roots[]={"MATERIAIS","MATERIAS","materiais","materias"}; int i;
        for(i=0;i<4;++i) {
            joinPath(path,sizeof(path),roots[i],relative);
            if(existsFile(path)) break;
        }
    }
    return loadObj(path,ship);
}

static void loadCityModels(void)
{
    const char *cars[]={"NormalCar1.obj","NormalCar2.obj","SportsCar.obj","SportsCar2.obj","SUV.obj","Taxi.obj","Cop.obj","Bus.obj"};
    char name[96]; int i,id;
    locateAssets();
    for(i=0;i<14;++i) {
        snprintf(name,sizeof(name),"building-%c.obj",'a'+i);
        id=modelFromAssets(name,0); if(id>=0) buildingModels[buildingCount++]=id;
    }
    for(i=0;i<5;++i) {
        snprintf(name,sizeof(name),"building-skyscraper-%c.obj",'a'+i);
        id=modelFromAssets(name,0); if(id>=0) buildingModels[buildingCount++]=id;
    }
    for(i=0;i<8;++i) { id=modelFromAssets(cars[i],0); if(id>=0) carModels[carCount++]=id; }
    coneModel=modelFromAssets("TrafficCone.obj",0);
    trafficModel=modelFromAssets("TrafficLight.obj",0);
    for(i=0;i<3;++i) { snprintf(name,sizeof(name),"TrafficSign%d.obj",i+1); signModels[i]=modelFromAssets(name,0); }
    shipModel=modelFromAssets("ship_pinnace_4k.obj",1);
    shipCameraAvailable=shipModel>=0;
    if(shipCameraAvailable) {
        Model *m=&models[shipModel];
        shipCameraModelHeight=(m->max.y-m->min.y)*SHIP_LENGTH/(m->max.z-m->min.z);
    }
}

static unsigned cityRand(void)
{
    cityRandom^=cityRandom<<13; cityRandom^=cityRandom>>17; cityRandom^=cityRandom<<5;
    return cityRandom;
}
static float randomRange(float min,float max)
{ return min+(max-min)*(cityRand()%10001)/10000.0f; }

static void modelBounds(int model,Vec3 position,float scale,float yaw,Vec3 *min,Vec3 *max)
{
    Model *m=&models[model]; float a=yaw*(float)(PI/180.0),c=cosf(a),s=sinf(a);
    float hx=fmaxf(fabsf(m->min.x),fabsf(m->max.x))*scale;
    float hz=fmaxf(fabsf(m->min.z),fabsf(m->max.z))*scale;
    float rx=fabsf(c)*hx+fabsf(s)*hz,rz=fabsf(s)*hx+fabsf(c)*hz;
    *min=vec(position.x-rx,position.y+m->min.y*scale,position.z-rz);
    *max=vec(position.x+rx,position.y+m->max.y*scale,position.z+rz);
}

static int addInstance(int model,float x,float y,float z,float scale,float yaw)
{
    Instance *p;
    if(model<0) return 0;
    if(instanceCount>=MAX_INSTANCES) { importFailed=1; return 0; }
    p=&instances[instanceCount++];
    p->model=model; p->position=vec(x,y,z); p->scale=scale; p->yaw=yaw;
    modelBounds(model,p->position,scale,yaw,&p->min,&p->max);
    return 1;
}

static void drawModel(int model,Vec3 position,float scale,float yaw)
{
    glPushMatrix();
    glTranslatef(position.x,position.y,position.z);
    glRotatef(yaw,0,1,0); glScalef(scale,scale,scale);
    glCallList(models[model].list);
    glPopMatrix();
}

/* Face vertical subdividida. a e b ficam na base, na ordem da face. */
static void wall(Vec3 a, Vec3 b, float height, Vec3 normal)
{
    float width = sqrtf(dot(sub(b,a),sub(b,a)));
    int nx = (int)ceilf(width/2.0f), ny = (int)ceilf(height/2.0f);
    int i,j;
    glNormal3f(normal.x,normal.y,normal.z);
    glBegin(GL_QUADS);
    for (j=0;j<ny;++j) for (i=0;i<nx;++i) {
        float u0=(float)i/nx, u1=(float)(i+1)/nx;
        float y0=a.y+height*j/ny, y1=a.y+height*(j+1)/ny;
        float x0=a.x+(b.x-a.x)*u0, z0=a.z+(b.z-a.z)*u0;
        float x1=a.x+(b.x-a.x)*u1, z1=a.z+(b.z-a.z)*u1;
        glTexCoord2f(u0*width/WALL_REPEAT,(y0-a.y)/WALL_REPEAT); glVertex3f(x0,y0,z0);
        glTexCoord2f(u1*width/WALL_REPEAT,(y0-a.y)/WALL_REPEAT); glVertex3f(x1,y0,z1);
        glTexCoord2f(u1*width/WALL_REPEAT,(y1-a.y)/WALL_REPEAT); glVertex3f(x1,y1,z1);
        glTexCoord2f(u0*width/WALL_REPEAT,(y1-a.y)/WALL_REPEAT); glVertex3f(x0,y1,z0);
    }
    glEnd();
}

static void box(float x,float y,float z,float w,float h,float d,
                float r,float g,float b)
{
    float x1=x+w,z1=z+d;
    glColor3f(r,g,b);
    wall(vec(x,y,z),vec(x1,y,z),h,vec(0,0,-1));
    wall(vec(x1,y,z),vec(x1,y,z1),h,vec(1,0,0));
    wall(vec(x1,y,z1),vec(x,y,z1),h,vec(0,0,1));
    wall(vec(x,y,z1),vec(x,y,z),h,vec(-1,0,0));
    /* Textura de parede somente nas fachadas; telhados continuam lisos. */
    glNormal3f(0,1,0);
    glBegin(GL_QUADS);
    glVertex3f(x,y+h,z); glVertex3f(x,y+h,z1);
    glVertex3f(x1,y+h,z1); glVertex3f(x1,y+h,z);
    glEnd();
}

static int nearRoadArray(float coordinate,const float *roads,float half)
{
    int i;
    for(i=0;i<4;++i) if(fabsf(coordinate-roads[i])<half) return 1;
    return 0;
}

static void ground(float minX,float minZ,float maxX,float maxZ)
{
    float x,z;
    if(floorTexture) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,floorTexture); }
    glNormal3f(0,1,0);
    glBegin(GL_QUADS);
    for(x=minX;x<maxX;x+=TILE)
    for(z=minZ;z<maxZ;z+=TILE) {
        float x1=minimum(x+TILE,maxX), z1=minimum(z+TILE,maxZ);
        float shade = (((int)(x+80)*17+(int)(z+80)*11)&3)*.003f;
        if(floorTexture) glColor3f(.55f+shade,.57f+shade,.60f+shade);
        else glColor3f(.17f+shade,.18f+shade,.19f+shade);
        glTexCoord2f(x/FLOOR_REPEAT,z/FLOOR_REPEAT); glVertex3f(x,0,z);
        glTexCoord2f(x/FLOOR_REPEAT,z1/FLOOR_REPEAT); glVertex3f(x,0,z1);
        glTexCoord2f(x1/FLOOR_REPEAT,z1/FLOOR_REPEAT); glVertex3f(x1,0,z1);
        glTexCoord2f(x1/FLOOR_REPEAT,z/FLOOR_REPEAT); glVertex3f(x1,0,z);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    /* Faixas curtas: nao atravessam os cruzamentos. */
    glColor3f(.55f,.49f,.30f);
    glBegin(GL_QUADS);
    { int i; float p;
      for(i=0;i<4;++i) for(p=-84;p<84;p+=6) {

        /* Cada faixa pertence ao trecho que contem seu centro.
         * A caixa de culling inclui margem para as faixas da borda. */
        if(!nearRoadArray(p,roadsZ,ROAD_HALF+1) && !nearRoadArray(p+2.5f,roadsZ,ROAD_HALF+1) &&
           roadsX[i]>=minX && roadsX[i]<maxX && p+1.25f>=minZ && p+1.25f<maxZ) {
            glVertex3f(roadsX[i]-.06f,.012f,p); glVertex3f(roadsX[i]+.06f,.012f,p);
            glVertex3f(roadsX[i]+.06f,.012f,p+2.5f); glVertex3f(roadsX[i]-.06f,.012f,p+2.5f);
        }
        if(!nearRoadArray(p,roadsX,ROAD_HALF+1) && !nearRoadArray(p+2.5f,roadsX,ROAD_HALF+1) &&
           p+1.25f>=minX && p+1.25f<maxX && roadsZ[i]>=minZ && roadsZ[i]<maxZ) {
            glVertex3f(p,.012f,roadsZ[i]-.06f); glVertex3f(p+2.5f,.012f,roadsZ[i]-.06f);
            glVertex3f(p+2.5f,.012f,roadsZ[i]+.06f); glVertex3f(p,.012f,roadsZ[i]+.06f);
        }
      }
    }
    glEnd();
}

static void lamp(float x,float z,float dx,float dz)
{
    float bx=x+dx*1.15f,bz=z+dz*1.15f;
    if(lampCount<MAX_LAMPS) {
        Lamp *l=&lamps[lampCount++];
        l->bulb=vec(bx,LAMP_HEIGHT-.15f,bz);
        l->intensity=1;
        l->random=(unsigned)lampCount*747796405u+2891336453u;
        l->min=vec(minimum(x-.09f,bx-.35f),.16f,minimum(z-.09f,bz-.35f));
        l->max=vec(-minimum(-x-.09f,-bx-.35f),LAMP_HEIGHT+.16f,
                   -minimum(-z-.09f,-bz-.35f));
        l->bodyList=glGenLists(1);
        if(!l->bodyList) return;
        glNewList(l->bodyList,GL_COMPILE);
    } else return;
    box(x-.09f,.16f,z-.09f,.18f,LAMP_HEIGHT,.18f,.20f,.22f,.23f);
    box(minimum(x,bx)-.08f,LAMP_HEIGHT,minimum(z,bz)-.08f,
        fabsf(x-bx)+.16f,.13f,fabsf(z-bz)+.16f,.23f,.24f,.25f);
    box(bx-.35f,LAMP_HEIGHT-.07f,bz-.35f,.7f,.20f,.7f,.23f,.24f,.25f);
    glEndList();
}

static unsigned randomLamp(Lamp *l)
{
    unsigned x=l->random;
    x^=x<<13; x^=x>>17; x^=x<<5;
    l->random=x;
    return x;
}

static void chooseFaultyLamps(void)
{
    int indices[MAX_LAMPS],i,count=(lampCount*FLICKER_PERCENT+50)/100;
    Lamp selection={0}; selection.random=0x1947b32du;
    for(i=0;i<lampCount;++i) indices[i]=i;
    /* Sorteio sem repeticao: quantidade arredondada de 20% do total. */
    for(i=lampCount-1;i>0;--i) {
        int j=(int)(randomLamp(&selection)%(unsigned)(i+1));
        int temp=indices[i]; indices[i]=indices[j]; indices[j]=temp;
    }
    for(i=0;i<count;++i) lamps[indices[i]].faulty=1;
    printf("Postes: %d | lampadas com defeito: %d\n",lampCount,count);
}

static void updateLamps(void)
{
    int i;
    for(i=0;i<lampCount;++i) {
        Lamp *l=&lamps[i];
        if(!l->faulty || animationTime<l->nextChange) continue;
        /* Periodos longos acesos, falhas curtas, apagamentos e brilho parcial.
         * Cada poste tem seu gerador/relogio; nao depende do FPS. */
        { unsigned r=randomLamp(l),state=r%100;
          double duration;
          if(state<40) { l->intensity=1; duration=.25+(randomLamp(l)%1800)/1000.0; }
          else if(state<76) { l->intensity=0; duration=.035+(randomLamp(l)%280)/1000.0; }
          else { l->intensity=.10f+(randomLamp(l)%50)/100.0f;
                 duration=.025+(randomLamp(l)%100)/1000.0; }
          l->nextChange=animationTime+duration;
        }
    }
}

/* Seis planos de P*V: teste conservador por caixa, sem cortar objetos que
 * cruzam a borda da imagem. Nao precisa normalizar os planos para AABBs. */
static void updateFrustum(void)
{
    GLfloat projection[16],view[16],clip[16];
    int row,column,k,axis;
    glGetFloatv(GL_PROJECTION_MATRIX,projection);
    glGetFloatv(GL_MODELVIEW_MATRIX,view);
    for(column=0;column<4;++column) for(row=0;row<4;++row) {
        clip[column*4+row]=0;
        for(k=0;k<4;++k)
            clip[column*4+row]+=projection[k*4+row]*view[column*4+k];
    }
    for(axis=0;axis<3;++axis) for(k=0;k<4;++k) {
        frustum[axis*2][k]=clip[k*4+3]+clip[k*4+axis];
        frustum[axis*2+1][k]=clip[k*4+3]-clip[k*4+axis];
    }
}

static int boxInFrustum(Vec3 min,Vec3 max)
{
    int i;
    for(i=0;i<6;++i) {
        const GLfloat *p=frustum[i];
        float x=p[0]>=0?max.x:min.x;
        float y=p[1]>=0?max.y:min.y;
        float z=p[2]>=0?max.z:min.z;
        if(p[0]*x+p[1]*y+p[2]*z+p[3]<-.01f) return 0;
    }
    return 1;
}

static int inView(Vec3 min,Vec3 max)
{ return !culling || boxInFrustum(min,max); }

/* Modelo compartilhado SEM cor nem emissao gravadas: apenas geometria. */
static void buildBulbGeometry(void)
{
    const float a=-.29f,b=.29f,h=.035f;
    bulbList=glGenLists(1);
    if(!bulbList) return;
    glNewList(bulbList,GL_COMPILE);
    glBegin(GL_QUADS);
    glNormal3f(0,0,-1);
    glVertex3f(a,0,a); glVertex3f(b,0,a); glVertex3f(b,h,a); glVertex3f(a,h,a);
    glNormal3f(1,0,0);
    glVertex3f(b,0,a); glVertex3f(b,0,b); glVertex3f(b,h,b); glVertex3f(b,h,a);
    glNormal3f(0,0,1);
    glVertex3f(b,0,b); glVertex3f(a,0,b); glVertex3f(a,h,b); glVertex3f(b,h,b);
    glNormal3f(-1,0,0);
    glVertex3f(a,0,b); glVertex3f(a,0,a); glVertex3f(a,h,a); glVertex3f(a,h,b);
    glNormal3f(0,1,0);
    glVertex3f(a,h,a); glVertex3f(a,h,b); glVertex3f(b,h,b); glVertex3f(b,h,a);
    glEnd();
    glEndList();
}

static void drawScene(void)
{
    int i;
    visibleParts=visibleLamps=0;
    for(i=0;i<partCount;++i) if(inView(parts[i].min,parts[i].max)) {
        glCallList(parts[i].list);
        ++visibleParts;
    }
    visibleInstances=0;
    for(i=0;i<instanceCount;++i) {
        Instance *p=&instances[i];
        if(!inView(p->min,p->max)) continue;
        drawModel(p->model,p->position,p->scale,p->yaw);
        ++visibleInstances;
    }
    for(i=0;i<lampCount;++i) {
        Lamp *l=&lamps[i];
        l->visible=inView(l->min,l->max);
        if(l->visible) { glCallList(l->bodyList); ++visibleLamps; }
    }
}

static void drawBulbs(void)
{
    int i;
    const GLfloat black[]={0,0,0,1};
    glDisable(GL_TEXTURE_2D);
    for(i=0;i<lampCount;++i) {
        Lamp *l=&lamps[i]; float v=l->intensity;
        GLfloat glow[]={v,.65f*v,.22f*v,1};
        if(!l->visible) continue;
        glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,glow);
        glColor3f(.10f+.90f*v,.08f+.67f*v,.04f+.31f*v);
        glPushMatrix();
        glTranslatef(l->bulb.x,LAMP_HEIGHT-.11f,l->bulb.z);
        glCallList(bulbList);
        glPopMatrix();
    }
    glMaterialfv(GL_FRONT_AND_BACK,GL_EMISSION,black);
}

static RenderPart *newPart(Vec3 min,Vec3 max)
{
    RenderPart *p;
    if(partCount>=MAX_PARTS) { importFailed=1; return NULL; }
    p=&parts[partCount++]; p->min=min; p->max=max; p->list=glGenLists(1);
    return p->list?p:NULL;
}

static void fillBlock(float x0,float z0,float x1,float z1)
{
    float width=x1-x0,depth=z1-z0;
    int nx=(int)((width-2)/10),nz=(int)((depth-2)/10),ix,iz;
    RenderPart *p;
    if(width<5 || depth<5) return;
    p=newPart(vec(x0,0,z0),vec(x1,.20f,z1));
    if(p) {
        glNewList(p->list,GL_COMPILE);
        box(x0,0,z0,width,.18f,depth,.38f,.39f,.40f);
        glEndList();
    }
    if(nx<1) nx=1;
    if(nz<1) nz=1;
    for(ix=0;ix<nx;++ix) for(iz=0;iz<nz;++iz) {
        int id,quarter=(int)(cityRand()%4);
        float cellW=width/nx,cellD=depth/nz,angle=quarter*90.0f;
        float w,d,scale,x,z;
        Model *m;
        /* Pequenas pracas/vazios somente no miolo dos quarteiroes grandes. */
        if(nx>2 && nz>2 && ix>0 && ix<nx-1 && iz>0 && iz<nz-1) continue;
        id=buildingModels[cityRand()%(unsigned)buildingCount]; m=&models[id];
        w=m->max.x-m->min.x; d=m->max.z-m->min.z;
        if(quarter&1) { float t=w; w=d; d=t; }
        scale=fminf((cellW-1.5f)/w,(cellD-1.5f)/d)*randomRange(.83f,.99f);
        scale=fminf(scale,55.0f/m->max.y);
        /* Mantem proporcoes originais; lotes variam de acordo com o bloco. */
        x=x0+(ix+.5f)*cellW; z=z0+(iz+.5f)*cellD;
        buildingInstances+=addInstance(id,x,.18f,z,scale,angle);
    }
}

static void parkedCar(int vertical,float road,float along,int side)
{
    int id; Model *m; float width,length,scale,yaw;
    if(!carCount) return;
    id=carModels[cityRand()%(unsigned)carCount]; m=&models[id];
    width=m->max.x-m->min.x; length=m->max.z-m->min.z;
    /* O onibus original aponta pelo eixo X, os outros carros pelo Z. */
    yaw=width>length?90.0f:0;
    scale=(strstr(m->name,"Bus")?6.2f:randomRange(3.8f,4.8f))/fmaxf(width,length);
    if(!vertical) yaw+=90;
    if(side>0) yaw+=180;
    carInstances+=addInstance(id,vertical?road+side*4.8f:along,.025f,
                             vertical?along:road+side*4.8f,scale,yaw);
}

static void populateStreetDetails(void)
{
    int axis,i,j,side; float p;
    for(axis=0;axis<2;++axis) for(i=0;i<4;++i) {
        float road=axis?roadsZ[i]:roadsX[i];
        const float *crossings=axis?roadsX:roadsZ;
        for(j=0;j<3;++j) {
            float begin=crossings[j]+10,end=crossings[j+1]-10;
            for(p=begin;p<end;p+=8) for(side=-1;side<=1;side+=2)
                if(cityRand()%100<65) parkedCar(!axis,road,p,side);
            if(coneModel>=0 && (cityRand()%2)==0) {
                float at=(begin+end)*.5f;
                int c;
                for(c=0;c<3;++c)
                    propInstances+=addInstance(coneModel,axis?at+c*1.1f:road+5.6f,
                        .02f,axis?road+5.6f:at+c*1.1f,
                        .72f/(models[coneModel].max.y-models[coneModel].min.y),0);
            }
        }
    }
    for(i=0;i<4;++i) for(j=0;j<4;++j) {
        int k=(int)(cityRand()%3),sign=signModels[k];
        float x=roadsX[i],z=roadsZ[j];
        if(trafficModel>=0) {
            float s=3.2f/models[trafficModel].max.y;
            propInstances+=addInstance(trafficModel,x+7.4f,.18f,z+7.4f,s,90);
            propInstances+=addInstance(trafficModel,x-7.4f,.18f,z-7.4f,s,270);
        }
        if(sign>=0) propInstances+=addInstance(sign,x-7.5f,.18f,z+8.8f,
                                              2.1f/models[sign].max.y,k==0?90.0f:0);
    }
}

static void buildScene(void)
{
    int ix,iz,axis,i,side;
    float x,z,p;
    float edgesX[]={-CITY_EDGE,-64,-30,12,64,CITY_EDGE};
    float edgesZ[]={-CITY_EDGE,-60,-22,20,60,CITY_EDGE};
    cityRandom=CITY_SEED?CITY_SEED:((unsigned)time(NULL)|1u);
    buildPath(); buildBulbGeometry();
    for(x=-CITY_EDGE;x<CITY_EDGE;x+=GROUND_CHUNK)
    for(z=-CITY_EDGE;z<CITY_EDGE;z+=GROUND_CHUNK) {
        float x1=minimum(x+GROUND_CHUNK,CITY_EDGE),z1=minimum(z+GROUND_CHUNK,CITY_EDGE);
        RenderPart *part=newPart(vec(x-1.3f,-.02f,z-1.3f),vec(x1+1.3f,.03f,z1+1.3f));
        if(!part) continue;
        glNewList(part->list,GL_COMPILE); ground(x,z,x1,z1); glEndList();
    }
    if(buildingCount) for(ix=0;ix<5;++ix) for(iz=0;iz<5;++iz) {
        float x0=edgesX[ix]+(ix?ROAD_HALF:1),x1=edgesX[ix+1]-(ix<4?ROAD_HALF:1);
        float z0=edgesZ[iz]+(iz?ROAD_HALF:1),z1=edgesZ[iz+1]-(iz<4?ROAD_HALF:1);
        fillBlock(x0,z0,x1,z1);
    }
    populateStreetDetails();
    for(axis=0;axis<2;++axis) for(i=0;i<4;++i) {
        const float *crossings=axis?roadsX:roadsZ;
        float road=axis?roadsZ[i]:roadsX[i];
        for(p=-80;p<=80;p+=13) {
            if(nearRoadArray(p,crossings,ROAD_HALF+2)) continue;
            for(side=-1;side<=1;side+=2) {
                if(axis) lamp(p,road+side*(ROAD_HALF+1.1f),0,(float)-side);
                else lamp(road+side*(ROAD_HALF+1.1f),p,(float)-side,0);
            }
        }
    }
    printf("Cidade: %d predios, %d carros, %d acessorios, %d postes\n",
           buildingInstances,carInstances,propInstances,lampCount);
}

static int sceneReady(void)
{
    int i;
    if(importFailed || !bulbList || !partCount || !lampCount || !buildingCount) return 0;
    for(i=0;i<partCount;++i) if(!parts[i].list) return 0;
    for(i=0;i<lampCount;++i) if(!lamps[i].bodyList) return 0;
    return 1;
}

static void destroyScene(void)
{
    int i;
    for(i=0;i<partCount;++i) if(parts[i].list) glDeleteLists(parts[i].list,1);
    for(i=0;i<lampCount;++i) if(lamps[i].bodyList) glDeleteLists(lamps[i].bodyList,1);
    for(i=0;i<modelCount;++i) if(models[i].list) glDeleteLists(models[i].list,1);
    for(i=0;i<textureCount;++i) if(textures[i].texture) glDeleteTextures(1,&textures[i].texture);
    if(bulbList) glDeleteLists(bulbList,1);
    if(floorTexture) glDeleteTextures(1,&floorTexture);
}

/* Seleciona fontes cujo poste/luminaria intercepta o frustum atual.
 * Testa a caixa do poste inteiro: ao olhar de cima a base pode estar visivel
 * enquanto o bulbo fica acima da borda da imagem. O teste independe da tecla C. Lampadas totalmente apagadas pelo defeito
 * cedem sua vaga; se houver menos de oito candidatas, usa apenas essas.
 * Uma pequena preferencia pelas fontes anteriores reduz trocas nas esquinas. */
static int activeLights;
static int previousLights[LIGHT_SLOTS]={-1,-1,-1,-1,-1,-1,-1,-1};
static void lighting(Vec3 eye)
{
    int selected[LIGHT_SLOTS],i,j,k;
    float scores[LIGHT_SLOTS];
    const GLfloat zero[]={0,0,0,1};
    GLfloat diffuse[]={LIGHT_POWER,LIGHT_POWER*.69f,LIGHT_POWER*.32f,1};
    activeLights=0;
    for(i=0;i<LIGHT_SLOTS;++i) { selected[i]=-1; scores[i]=1e30f; }
    for(i=0;i<lampCount;++i) {
        Vec3 delta; float score;
        if(lamps[i].intensity<=.01f || !boxInFrustum(lamps[i].min,lamps[i].max)) continue;
        delta=sub(lamps[i].bulb,eye); score=dot(delta,delta);
        for(k=0;k<LIGHT_SLOTS;++k) if(previousLights[k]==i) { score*=.85f; break; }
        for(j=0;j<LIGHT_SLOTS;++j) if(score<scores[j]) {
            for(k=LIGHT_SLOTS-1;k>j;--k) { scores[k]=scores[k-1]; selected[k]=selected[k-1]; }
            scores[j]=score; selected[j]=i; break;
        }
    }
    for(i=0;i<LIGHT_SLOTS;++i) {
        GLenum id=GL_LIGHT0+i;
        previousLights[i]=selected[i];
        if(selected[i]<0) { glDisable(id); continue; }
        { Vec3 p=lamps[selected[i]].bulb; GLfloat position[]={p.x,p.y,p.z,1};
          float v=lamps[selected[i]].intensity;
          glEnable(id); glLightfv(id,GL_POSITION,position);
          diffuse[0]=LIGHT_POWER*v; diffuse[1]=LIGHT_POWER*.69f*v; diffuse[2]=LIGHT_POWER*.32f*v;
        }
        ++activeLights;
        glLightfv(id,GL_AMBIENT,zero); glLightfv(id,GL_DIFFUSE,diffuse); glLightfv(id,GL_SPECULAR,zero);
        glLightf(id,GL_CONSTANT_ATTENUATION,.65f); glLightf(id,GL_LINEAR_ATTENUATION,.08f);
        glLightf(id,GL_QUADRATIC_ATTENUATION,.018f);
    }
}

static int shipVisible;
static void drawShip(void)
{
    double distance=132.0-animationTime*SHIP_SPEED;
    Vec3 p,next,direction,min,max; float scale,yaw;
    shipVisible=0;
    if(shipModel<0) return;
    p=route(distance); next=route(distance-.5); direction=unit(sub(next,p));
    p=shipPosition(animationTime);
    scale=SHIP_LENGTH/(models[shipModel].max.z-models[shipModel].min.z);
    yaw=(float)(atan2(direction.x,direction.z)*180.0/PI)+SHIP_YAW_OFFSET;
    modelBounds(shipModel,p,scale,yaw,&min,&max);
    /* Apenas informa a posicao no titulo; NAO descarta o navio.
     * O draw call acontece sempre, com C ligado ou desligado.
     * Profundidade, neblina e recorte normal do OpenGL continuam valendo. */
    shipVisible=boxInFrustum(min,max);
    drawModel(shipModel,p,scale,yaw);
}

/* Alterna o MESMO contexto: modelos/texturas/display lists permanecem validos.
 * Em tela cheia pede o modo atual completo do monitor, sem esticar 1024x768. */
static int fullscreen,windowX=100,windowY=100,windowWidth=1024,windowHeight=768;
static int resetFrameStats;
static double displayedFPS,lastMouseClick=-1,lastMouseX,lastMouseY;

static GLFWmonitor *windowMonitor(GLFWwindow *window)
{
    GLFWmonitor **monitors,*best=glfwGetPrimaryMonitor();
    int count=0,i,x,y,w,h; long bestArea=0;
    glfwGetWindowPos(window,&x,&y); glfwGetWindowSize(window,&w,&h);
    monitors=glfwGetMonitors(&count);
    for(i=0;i<count;++i) {
        int mx,my,left,top,right,bottom;
        const GLFWvidmode *mode=glfwGetVideoMode(monitors[i]);
        if(!mode) continue;
        glfwGetMonitorPos(monitors[i],&mx,&my);
        left=x>mx?x:mx; top=y>my?y:my;
        right=x+w<mx+mode->width?x+w:mx+mode->width;
        bottom=y+h<my+mode->height?y+h:my+mode->height;
        if(right>left && bottom>top) {
            long area=(long)(right-left)*(bottom-top);
            if(area>bestArea) { bestArea=area; best=monitors[i]; }
        }
    }
    return best;
}

static void toggleFullscreen(GLFWwindow *window)
{
    if(glfwGetWindowMonitor(window)) {
        glfwSetWindowMonitor(window,NULL,windowX,windowY,windowWidth,windowHeight,GLFW_DONT_CARE);
    } else {
        GLFWmonitor *monitor=windowMonitor(window);
        const GLFWvidmode *mode=monitor?glfwGetVideoMode(monitor):NULL;
        if(!mode) return;
        glfwGetWindowPos(window,&windowX,&windowY);
        glfwGetWindowSize(window,&windowWidth,&windowHeight);
        glfwSetWindowMonitor(window,monitor,0,0,mode->width,mode->height,mode->refreshRate);
    }
    fullscreen=glfwGetWindowMonitor(window)!=NULL;
    glfwSwapInterval(vsync);
    resetFrameStats=1; lastMouseClick=-1;
}

static void mouseButton(GLFWwindow *window,int button,int action,int mods)
{
    double now,x,y,dx,dy;
    (void)mods;
    if(button!=GLFW_MOUSE_BUTTON_LEFT || action!=GLFW_PRESS) return;
    now=glfwGetTime(); glfwGetCursorPos(window,&x,&y);
    dx=x-lastMouseX; dy=y-lastMouseY;
    /* Dois cliques proximos, sem confundir cliques separados/arrastos. */
    if(lastMouseClick>=0 && now-lastMouseClick<=.35 && dx*dx+dy*dy<=36.0) {
        toggleFullscreen(window);
        lastMouseClick=-1;
    } else { lastMouseClick=now; lastMouseX=x; lastMouseY=y; }
}

/* Fonte 5x7 embutida em C: sem fonte externa, GDI, GLU ou textura extra. */
static const unsigned char hudFont[128][7]={
    ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31}, ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
    ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14}, ['9']={14,17,17,15,1,1,14},
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
    ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
    ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17},
    ['I']={14,4,4,4,4,4,14}, ['J']={7,2,2,2,2,18,12},
    ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17}, ['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14}, ['P']={30,17,17,30,16,16,16},
    ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
    ['U']={17,17,17,17,17,17,14}, ['V']={17,17,17,17,17,10,4},
    ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31},
    ['.']={0,0,0,0,0,12,12}, [':']={0,12,12,0,12,12,0},
    ['[']={14,8,8,8,8,8,14}, [']']={14,2,2,2,2,2,14},
    ['-']={0,0,0,31,0,0,0}, ['/']={1,2,2,4,8,8,16}
};

/* Chamado com GL_QUADS aberto; um unico lote para os pixels de cada HUD. */
static void hudText(float x,float y,int scale,const char *text)
{
    int row,column; unsigned char ch;
    while((ch=(unsigned char)*text++)!=0) {
        if(ch<128) for(row=0;row<7;++row) for(column=0;column<5;++column)
            if(hudFont[ch][row]&(1<<(4-column))) {
                float px=x+column*scale,py=y+row*scale;
                glVertex2f(px,py); glVertex2f(px+scale,py);
                glVertex2f(px+scale,py+scale); glVertex2f(px,py+scale);
            }
        x+=6*scale;
    }
}

static void drawFullscreenHUD(int width,int height)
{
    char lines[4][64]; int i,longest=0,scale=height>=1800?3:2;
    float padding=5.0f*scale,margin=7.0f*scale,lineHeight=10.0f*scale;
    float x,y=margin,panelWidth,panelHeight;
    if(!fullscreen || width<=0 || height<=0) return;
    snprintf(lines[0],sizeof(lines[0]),"RES %d X %d",width,height);
    snprintf(lines[1],sizeof(lines[1]),"FPS %.1f",displayedFPS);
    snprintf(lines[2],sizeof(lines[2]),"CULLING %s [C]",culling?"ON":"OFF");
    snprintf(lines[3],sizeof(lines[3]),"VSYNC %s [V]",vsync?"ON":"OFF");
    for(i=0;i<4;++i) if((int)strlen(lines[i])>longest) longest=(int)strlen(lines[i]);
    panelWidth=longest*6.0f*scale+padding*2;
    panelHeight=4*lineHeight-3*scale+padding*2;
    x=width-margin-panelWidth;
    glPushAttrib(GL_ENABLE_BIT|GL_COLOR_BUFFER_BIT|GL_CURRENT_BIT|GL_DEPTH_BUFFER_BIT|GL_POLYGON_BIT|GL_TRANSFORM_BIT);
    glDisable(GL_LIGHTING); glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_FOG); glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D); glDisable(GL_CULL_FACE); glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0,width,height,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glColor4f(.015f,.02f,.03f,.78f);
    glBegin(GL_QUADS);
    glVertex2f(x,y); glVertex2f(x+panelWidth,y);
    glVertex2f(x+panelWidth,y+panelHeight); glVertex2f(x,y+panelHeight);
    glEnd();
    glBegin(GL_QUADS);
    for(i=0;i<4;++i) {
        if(i<2) glColor4f(.92f,.95f,1,1);
        else if((i==2 && culling)||(i==3 && vsync)) glColor4f(.55f,.94f,.72f,1);
        else glColor4f(1,.75f,.47f,1);
        hudText(x+padding,y+padding+i*lineHeight,scale,lines[i]);
    }
    glEnd();
    glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
    glPopAttrib(); /* Inclusive o modo de matriz e o aramado anterior. */
}

static void key(GLFWwindow *w,int code,int scan,int action,int mods)
{
    (void)scan;
    if(action!=GLFW_PRESS) return;
    if((code==GLFW_KEY_ENTER || code==GLFW_KEY_KP_ENTER) && (mods&GLFW_MOD_ALT)) {
        toggleFullscreen(w); return;
    }
    switch(code) {
    case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(w,GLFW_TRUE); break;
    case GLFW_KEY_SPACE: paused=!paused; break;
    case GLFW_KEY_R:
        travel=animationTime=0;
        resetShipCamera();
        { int i; for(i=0;i<lampCount;++i) {
            lamps[i].intensity=1; lamps[i].nextChange=0;
        } }
        break;
    case GLFW_KEY_V: vsync=!vsync; glfwSwapInterval(vsync); break;
    case GLFW_KEY_F: wireframe=!wireframe; break;
    case GLFW_KEY_C: culling=!culling; break;
    case GLFW_KEY_UP: speed=fmin(speed+1,20); break;
    case GLFW_KEY_DOWN: speed=fmax(speed-1,1); break;
    }
}
static void errorCallback(int code,const char *description)
{ fprintf(stderr,"GLFW (%d): %s\n",code,description); }

int main(void)
{
    GLFWwindow *window;
    const GLfloat black[]={0,0,0,1};
    double previous,statsTime; int frames=0;
    glfwSetErrorCallback(errorCallback);
    if(!glfwInit()) return 1;
    /* Nao pedir CORE profile: glBegin, luzes e matrizes usam pipeline fixo. */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,1);
    glfwWindowHint(GLFW_RESIZABLE,GLFW_FALSE);
    glfwWindowHint(GLFW_DEPTH_BITS,24);
    window=glfwCreateWindow(1024,768,"Night Street - OpenGL retro",NULL,NULL);
    if(!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
#ifdef _WIN32
    /* Recursos relativos ao executavel, mesmo ao iniciar por atalho. */
    { wchar_t directory[32768]; DWORD n=GetModuleFileNameW(NULL,directory,32768);
      if(n>0 && n<32768) { wchar_t *slash=wcsrchr(directory,L'\\');
          if(slash) { *slash=0; SetCurrentDirectoryW(directory); }
      }
    }
#endif
    glfwSwapInterval(vsync);
    glfwSetKeyCallback(window,key);
    glfwSetMouseButtonCallback(window,mouseButton);
    printf("OpenGL: %s\nGPU: %s\n",(const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_RENDERER));
    printf("ESC sair | ESPACO pausar | R reiniciar | V VSync | F aramado | C culling\nAlt+Enter ou duplo clique esquerdo: tela cheia/janela\n");
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE); /* Modelos reutilizados com escalas uniformes. */
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT,black);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER,GL_TRUE);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE,GL_EXP2);
    glFogfv(GL_FOG_COLOR,black);
    glFogf(GL_FOG_DENSITY,FOG_DENSITY);
    glHint(GL_FOG_HINT,GL_NICEST);
    glClearColor(0,0,0,1);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    if(USE_TEXTURES) {
        floorTexture=loadTexture(L"floor.jpg");
    }
    loadCityModels();
    buildScene();
    chooseFaultyLamps();
    if(!sceneReady()) {
        fprintf(stderr,"Falha ao montar a cidade. Confira MATERIAIS/predios e as mensagens acima.\n");
        destroyScene(); glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    previous=statsTime=glfwGetTime();
    while(!glfwWindowShouldClose(window)) {
        double now=glfwGetTime(),dt=now-previous;
        int width,height; Vec3 eye,target;
        previous=now;
        glfwPollEvents();
        fullscreen=glfwGetWindowMonitor(window)!=NULL;
        if(resetFrameStats) {
            previous=now=statsTime=glfwGetTime();
            frames=0; dt=0; displayedFPS=0; resetFrameStats=0;
        }
        /* Limita saltos apos arrastar/minimizar a janela. */
        if(dt>.1) dt=.1;
        if(!paused) { travel+=speed*dt; animationTime+=dt; }
        updateLamps();
        eye=route(travel); target=route(travel+3.0);
        animateCamera(&eye,&target);
        glfwGetFramebufferSize(window,&width,&height);
        if(width<=0||height<=0) { glfwWaitEventsTimeout(.05); continue; }
        glViewport(0,0,width,height);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        { double top=.1*tan(65.0*PI/360.0), right=top*(double)width/height;
          glFrustum(-right,right,-top,top,.1,150); }
        glMatrixMode(GL_MODELVIEW);
        camera(eye,target);
        updateFrustum();
        lighting(eye);
        glPolygonMode(GL_FRONT_AND_BACK,wireframe?GL_LINE:GL_FILL);
        drawScene();
        drawShip();
        drawBulbs();
        drawFullscreenHUD(width,height);
        glfwSwapBuffers(window);
        ++frames;
        if(now-statsTime>=.5) {
            char title[400];
            displayedFPS=frames/(now-statsTime);
            snprintf(title,sizeof(title),
                "Night Street | %.1f FPS | VSync %s | Culling %s | Modelos %d/%d | Luzes %d/8 | Navio %s | %s",
                displayedFPS,vsync?"ON":"OFF",culling?"ON":"OFF",
                visibleInstances,instanceCount,activeLights,shipVisible?"visivel":"fora da tela",paused?"PAUSA":"AUTO");
            glfwSetWindowTitle(window,title); frames=0; statsTime=now;
        }
    }
    destroyScene();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
