/*
 * swas — Sway App Selector: a radial (weapon-wheel) app launcher for Wayland.
 *
 * Written/modified by: Claude Opus 4.1, Claude Sonnet 4.5, Claude Opus 5
 *
 * Dependencies:
 *   - libwayland-client  (the only library you link)
 *   - sw_draw.h / sw_wl.h / sw_keymap.h  (in-tree: rasteriser, layer-shell
 *     window, keymap — shared with swbr and swov)
 *   - stb_truetype.h  (vendored single-header — smooth fonts)
 *   - stb_image.h     (vendored single-header — PNG/JPG/BMP icons)
 *   - nanosvg.h + nanosvgrast.h  (vendored single-header — SVG icons)
 * The vendored *.h files just sit next to this source; nothing to install/link.
 *
 * Build:
 *   cc swas.c -o swas -lwayland-client -lm
 *
 * See --help for every option (all config keys also work on the command line).
 */

/* ---------------------------------------------------------------------------
 * How this file is laid out (top to bottom):
 *   1. small helpers        colors, string trim, hex-color + tilde parsing
 *   2. Config               the struct, defaults, key=value setter, file loader
 *   3. text (Font)          stb_truetype atlas + drawing, with a debug fallback
 *   4. AppList              .desktop parsing, dir scanning, include/exclude/sort
 *   5. icons                Icon= resolution + stb_image / nanosvg (lazy, cached)
 *   6. launching + history  detached exec, most-recently-used log
 *   7. geometry             circle/sector/chevron primitives
 *   8. usage()/dump_config  --help text and the default config generator
 *   9. main()               setup, then the event + render loop
 * ------------------------------------------------------------------------- */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"

#include "sw_theme.h"

/* The window, the canvas and the keyboard. swas draws every pixel itself, so
   there is no renderer and no GPU: a layer-shell surface with a shared memory
   buffer, filled in by hand. */
#define SW_DRAW_IMPL
#include "sw_draw.h"
typedef struct { float x,y,w,h; } Rect;
#define SW_KEYMAP_IMPL
#include "sw_keymap.h"
#define SW_WL_IMPL
#include "sw_wl.h"

#define APP_ID "swas"          /* wayland app_id / X11 WM_CLASS */

#define SWAS_VERSION "1.3.3"
#ifndef SWAS_BUILD             /* set by the Makefile: md5 of this file */
#define SWAS_BUILD "unknown"
#endif

/* SVG icons via nanosvg (vendored single-header). Third-party headers, so we
   quiet their warnings without affecting our own -Wall build. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <glob.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (M_PI / 180.0)
#define ICON_AL (120.0f*(float)DEG)   /* bottom-left paging icon  */
#define ICON_AR (60.0f*(float)DEG)    /* bottom-right paging icon */

/* ------------------------------------------------------------------ */
/* Col comes from sw_draw.h — the canvas and this file agree on it. */
static char *xstrdup(const char *s){ char*p=malloc(strlen(s)+1); if(p)strcpy(p,s); return p; }
static char *trim(char *s){
    while(*s && isspace((unsigned char)*s)) s++;
    if(!*s) return s;
    char*e=s+strlen(s)-1; while(e>s && isspace((unsigned char)*e)) *e--='\0';
    return s;
}
static int contains_ci(const char*hay,const char*needle){
    if(!*needle) return 1;
    size_t nl=strlen(needle);
    for(const char*p=hay;*p;p++) if(strncasecmp(p,needle,nl)==0) return 1;
    return 0;
}
static void parse_color(const char*s, Col*out){
    if(*s=='#') s++;
    unsigned r,g,b,a=255;
    if(strlen(s)>=8 && sscanf(s,"%2x%2x%2x%2x",&r,&g,&b,&a)==4){}
    else if(sscanf(s,"%2x%2x%2x",&r,&g,&b)==3){}
    else return;
    out->r=(uint8_t)r; out->g=(uint8_t)g; out->b=(uint8_t)b; out->a=(uint8_t)a;
}
static int file_exists(const char*p){ struct stat st; return stat(p,&st)==0 && S_ISREG(st.st_mode); }
static float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }
/* expand a leading ~/ to $HOME (no shell involved when we launch) */
static void expand_tilde(const char*in,char*out,size_t n){
    if(in[0]=='~' && (in[1]=='/'||in[1]=='\0')){
        const char*home=getenv("HOME");
        if(home){ snprintf(out,n,"%s%s",home,in+1); return; }
    }
    snprintf(out,n,"%s",in);
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int   width,height,fullscreen,close_on_focus_loss;
    int   slots; float arc_deg; int page_ms;
    float radius, y_offset;           /* wheel size + vertical nudge (of min dim) */
    int   icons, icon_px;
    float ui_scale; int font_px;
    float label_px, title_px, search_px, count_px;   /* per-element text sizes */
    int   ssaa;                       /* full-scene supersample (1..4) */
    int   animate, anim_ms;           /* app-data crossfade on results change */
    int   recent_first;               /* bias the best match toward recently-used apps */
    char  font[PATH_MAX];
    char  sort[16], launcher[16], terminal[128];
    char  history[PATH_MAX], dirs[4096], include[8192], exclude[8192];
    int   drop;                       /* drag an app onto a workspace: 0 = off */
    int   center_first;               /* the most recent app at the top, the
                                         rest spreading either side of it     */
    int   overview;                   /* 1 = swov behind the wheel as the target */
    int   overview_debug;             /* narrate the backdrop conversation      */
    float drop_shrink;                /* how small the wheel gets while dragging */
    int   drop_ms, drop_px, drop_focus;
    int   drop_assign;                /* assign the pid before the app opens,
                                         or let it open here and move it     */
    int   drop_close_here;            /* dropped on the workspace we are on:
                                         close, or the app opens behind us   */
    char  drop_corner[16];            /* where the wheel sits while dragging:
                                         center, top-left, top-right,
                                         bottom-left, bottom-right, none     */
    int   drop_size;                  /* its width in px then; 0 = use
                                         drop_shrink as a fraction instead   */
    char  swov[PATH_MAX];             /* the binary that knows about sway */
    Col bg,ring,ring2,hl,text,hltext,center,accent,dim;
    Col drop_bg;                      /* the disc behind the shrunken wheel */
} Config;

/* ~/.config/swas/..., falling back to the old ~/.config/appwheel/... so an
   existing config and history keep working after the rename */
static void xdg_path(char*out,size_t n,const char*envvar,const char*dotdir,
                     const char*leaf){
    const char*base=getenv(envvar); const char*home=getenv("HOME");
    char legacy[PATH_MAX];
    if(base&&*base){ snprintf(out,n,"%s/swas/%s",base,leaf);
                     snprintf(legacy,sizeof legacy,"%s/appwheel/%s",base,leaf); }
    else if(home){   snprintf(out,n,"%s/%s/swas/%s",home,dotdir,leaf);
                     snprintf(legacy,sizeof legacy,"%s/%s/appwheel/%s",home,dotdir,leaf); }
    else { out[0]='\0'; return; }
    if(access(out,F_OK)!=0&&access(legacy,F_OK)==0) snprintf(out,n,"%s",legacy);
}

static void config_defaults(Config*c){
    memset(c,0,sizeof *c);
    c->width=900; c->height=900; c->fullscreen=1; c->close_on_focus_loss=0;
    c->slots=11; c->arc_deg=240.0f; c->page_ms=110;
    c->radius=0.44f; c->y_offset=0.10f;
    c->icons=1; c->icon_px=46; c->ui_scale=1.0f; c->font_px=50;
    c->label_px=24; c->title_px=25; c->search_px=20; c->count_px=20;
    c->ssaa=2;
    c->animate=1; c->anim_ms=90; c->recent_first=1;
    c->drop=1; c->drop_shrink=0.55f; c->drop_ms=130; c->drop_px=10; c->drop_assign=1; c->drop_focus=0;
    c->center_first=1; c->overview=0; c->overview_debug=0; c->drop_close_here=0;
    strcpy(c->drop_corner,"center"); c->drop_size=66;
    strcpy(c->swov,"swov");
    strcpy(c->sort,"recent"); strcpy(c->launcher,"sh");
    const char*term=getenv("TERMINAL");
    snprintf(c->terminal,sizeof c->terminal,"%s",term?term:"xterm");

    xdg_path(c->history,sizeof c->history,"XDG_CACHE_HOME",".cache","history");
    c->bg    =(Col){0x0d,0x11,0x17,0x00};   /* transparent by default */
    c->ring  =(Col){0x1e,0x27,0x33,0xf2};
    c->ring2 =(Col){0x26,0x31,0x3f,0xf2};
    c->hl    =(Col){0xcb,0x9b,0x00,0xff};
    c->text  =(Col){0xe8,0xe8,0xe8,0xff};
    c->hltext=(Col){0x14,0x14,0x14,0xff};
    c->center=(Col){0x0d,0x11,0x17,0xe6};
    c->drop_bg=(Col){0x0d,0x11,0x17,0xd9};
    c->accent=(Col){0x89,0xaf,0xc4,0xff};
    c->dim   =(Col){0x5a,0x6b,0x7a,0xff};
}
static void config_set(Config*c,const char*k,const char*v){
    if(!strcmp(k,"width"))c->width=atoi(v);
    else if(!strcmp(k,"height"))c->height=atoi(v);
    else if(!strcmp(k,"fullscreen"))c->fullscreen=atoi(v);
    else if(!strcmp(k,"close_on_focus_loss"))c->close_on_focus_loss=atoi(v);
    else if(!strcmp(k,"slots"))c->slots=atoi(v);
    else if(!strcmp(k,"arc")||!strcmp(k,"arc_deg"))c->arc_deg=(float)atof(v);
    else if(!strcmp(k,"page_ms"))c->page_ms=atoi(v);
    else if(!strcmp(k,"radius")||!strcmp(k,"size"))c->radius=(float)atof(v);
    else if(!strcmp(k,"y_offset")||!strcmp(k,"y"))c->y_offset=(float)atof(v);
    else if(!strcmp(k,"icons"))c->icons=atoi(v);
    else if(!strcmp(k,"icon_px"))c->icon_px=atoi(v);
    else if(!strcmp(k,"ui_scale")||!strcmp(k,"font_scale")||!strcmp(k,"text_scale"))c->ui_scale=(float)atof(v);
    else if(!strcmp(k,"font_px"))c->font_px=atoi(v);
    else if(!strcmp(k,"label_px"))c->label_px=(float)atof(v);
    else if(!strcmp(k,"title_px"))c->title_px=(float)atof(v);
    else if(!strcmp(k,"search_px")||!strcmp(k,"query_px"))c->search_px=(float)atof(v);
    else if(!strcmp(k,"count_px"))c->count_px=(float)atof(v);
    else if(!strcmp(k,"ssaa")||!strcmp(k,"aa"))c->ssaa=atoi(v);
    else if(!strcmp(k,"animate"))c->animate=atoi(v);
    else if(!strcmp(k,"anim_ms"))c->anim_ms=atoi(v);
    else if(!strcmp(k,"recent_first"))c->recent_first=atoi(v);
    else if(!strcmp(k,"drop"))c->drop=atoi(v);
    else if(!strcmp(k,"center_first"))c->center_first=atoi(v);
    else if(!strcmp(k,"overview"))c->overview=atoi(v);
    else if(!strcmp(k,"overview_debug"))c->overview_debug=atoi(v);
    else if(!strcmp(k,"drop_close_here"))c->drop_close_here=atoi(v);
    else if(!strcmp(k,"drop_corner")||!strcmp(k,"drop_pos"))
        snprintf(c->drop_corner,sizeof c->drop_corner,"%s",v);
    else if(!strcmp(k,"drop_size"))c->drop_size=atoi(v);
    else if(!strcmp(k,"drop_shrink"))c->drop_shrink=(float)atof(v);
    else if(!strcmp(k,"drop_ms"))c->drop_ms=atoi(v);
    else if(!strcmp(k,"drop_px"))c->drop_px=atoi(v);
    else if(!strcmp(k,"drop_assign"))c->drop_assign=atoi(v);
    else if(!strcmp(k,"drop_focus"))c->drop_focus=atoi(v);
    else if(!strcmp(k,"swov"))snprintf(c->swov,sizeof c->swov,"%s",v);
    else if(!strcmp(k,"font"))snprintf(c->font,sizeof c->font,"%s",v);
    else if(!strcmp(k,"sort"))snprintf(c->sort,sizeof c->sort,"%s",v);
    else if(!strcmp(k,"launcher"))snprintf(c->launcher,sizeof c->launcher,"%s",v);
    else if(!strcmp(k,"terminal"))snprintf(c->terminal,sizeof c->terminal,"%s",v);
    else if(!strcmp(k,"history"))snprintf(c->history,sizeof c->history,"%s",v);
    else if(!strcmp(k,"dirs"))snprintf(c->dirs,sizeof c->dirs,"%s",v);
    else if(!strcmp(k,"include"))snprintf(c->include,sizeof c->include,"%s",v);
    else if(!strcmp(k,"exclude"))snprintf(c->exclude,sizeof c->exclude,"%s",v);
    else if(!strcmp(k,"bg"))parse_color(v,&c->bg);
    else if(!strcmp(k,"ring"))parse_color(v,&c->ring);
    else if(!strcmp(k,"ring2"))parse_color(v,&c->ring2);
    else if(!strcmp(k,"hl"))parse_color(v,&c->hl);
    else if(!strcmp(k,"text"))parse_color(v,&c->text);
    else if(!strcmp(k,"hltext"))parse_color(v,&c->hltext);
    else if(!strcmp(k,"center"))parse_color(v,&c->center);
    else if(!strcmp(k,"drop_bg"))parse_color(v,&c->drop_bg);
    else if(!strcmp(k,"accent"))parse_color(v,&c->accent);
    else if(!strcmp(k,"dim"))parse_color(v,&c->dim);
    else fprintf(stderr,"wheel: unknown key '%s'\n",k);
}
/* the shared ~/.config/sw/config, translated into swas's own keys */
static void config_set_shared(void*ud,const char*k,const char*v){ config_set((Config*)ud,k,v); }

/* `key=value   # what it does` — the trailing comment is not part of the
 * value. Numbers survived it by accident (atoi stops at the space) but a
 * string key did not: `sort=recent  # ...` set sort to the whole rest of the
 * line, which is not "recent", and the most-recently-used order quietly
 * turned itself off. A '#' only starts a comment after whitespace, so a value
 * may still contain one. */
static void strip_comment(char*v){
    char q=0;
    for(char*p=v;*p;p++){
        if(q){ if(*p==q) q=0; continue; }
        if(*p=='\''||*p=='"'){ q=*p; continue; }
        if(*p=='#'&&p>v&&isspace((unsigned char)p[-1])){ *p='\0'; return; }
    }
}

static void config_load(Config*c,const char*path){
    FILE*f=fopen(path,"r"); if(!f) return;
    char line[8192];
    while(fgets(line,sizeof line,f)){
        char*s=trim(line); if(!*s||*s=='#'||*s==';') continue;
        char*eq=strchr(s,'='); if(!eq) continue;
        *eq='\0';
        char*v=eq+1; strip_comment(v);
        config_set(c,trim(s),trim(v));
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* text: sw_draw's glyph cache                                         */
/* ------------------------------------------------------------------ */
/* There used to be an atlas here: every glyph rasterised once at one size
   into a texture, then scaled. sw_draw caches per pixel size instead and
   rasterises a glyph the first time it is asked for, so text at any size is
   sharp rather than a scaled-up copy of one size. What is left is the old
   call shape, because a hundred call sites are written against it. */
typedef struct { int ok; int px; } UiFont;

static int try_font_paths(char*out,size_t n){
    const char*cand[]={
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf", NULL };
    for(int i=0;cand[i];i++) if(file_exists(cand[i])){ snprintf(out,n,"%s",cand[i]); return 1; }
    return 0;
}

/* Ask fontconfig (via its fc-match CLI) to resolve a family/pattern — e.g.
   "sans-serif", "monospace", or "JetBrains Mono" — to an actual .ttf path.
   This is how we pick up the DESKTOP's configured default font. No library is
   linked; if fc-match isn't installed we just return 0 and fall back. */
static int fc_match(const char*pattern,char*out,size_t n){
    if(!pattern||!*pattern) pattern="sans-serif";
    char safe[256]; size_t j=0;                    /* strip shell metacharacters */
    for(size_t i=0;pattern[i]&&j<sizeof safe-1;i++){
        char c=pattern[i];
        if(c=='"'||c=='`'||c=='$'||c=='\\'||c==';'||c=='|'||c=='&'||c=='\n'||c=='\r') continue;
        safe[j++]=c;
    }
    safe[j]='\0';
    char cmd[512];
    snprintf(cmd,sizeof cmd,"fc-match --format=%%{file} \"%s\" 2>/dev/null",safe);
    FILE*p=popen(cmd,"r"); if(!p) return 0;
    char buf[PATH_MAX]; size_t r=fread(buf,1,sizeof buf-1,p); buf[r]='\0';
    pclose(p);
    char*s=trim(buf);
    if(*s && file_exists(s)){ snprintf(out,n,"%s",s); return 1; }
    return 0;
}

static int font_load(UiFont*ft,const char*path,int px){
    ft->ok=0; ft->px=px;
    sw_fonts_init(path,NULL);
    UiFont *probe=ft;                       /* a font with no glyphs is no font */
    if(sw_text_width(sw_font(px),"M")>0.0f) probe->ok=1;
    return ft->ok;
}

/* utf8_next lived here; sw_draw walks the string itself now. */

static float text_width(UiFont*ft,float px_h,const char*s){
    (void)ft;
    int px=(int)(px_h+0.5f); if(px<4)px=4;
    return sw_text_width(sw_font(px),s);
}
/* y is the top of the line, as it always was here; sw_text wants a baseline. */
static void text_draw(Canvas*cv,UiFont*ft,float x,float y,float px_h,Col c,const char*s){
    (void)ft;
    int px=(int)(px_h+0.5f); if(px<4)px=4;
    Font *f=sw_font(px);
    sw_text(cv,f,x,y+f->ascent,c,s);
}
static void text_centered(Canvas*cv,UiFont*ft,float cx,float cy,float px_h,Col c,const char*s){
    float w=text_width(ft,px_h,s); text_draw(cv,ft,cx-w/2,cy-px_h/2,px_h,c,s);
}
static void fit_label(UiFont*ft,float px_h,const char*s,float max_w,char*out,size_t n){
    if(text_width(ft,px_h,s)<=max_w){ snprintf(out,n,"%s",s); return; }
    char buf[256]; snprintf(buf,sizeof buf,"%s",s); int len=(int)strlen(buf);
    while(len>1){ buf[len]='\0';
        char t[260]; snprintf(t,sizeof t,"%.*s..",len,buf);
        if(text_width(ft,px_h,t)<=max_w){ snprintf(out,n,"%.*s",(int)n-1,t); return; }
        len--;
    }
    snprintf(out,n,"..");
}

/* ------------------------------------------------------------------ */
/* application list                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    char *id,*name,*exec,*icon; int terminal;
    Image tex; int icon_tried; int recent;   /* 1 = present in launch history */
} App;
typedef struct { App*v; int n,cap; } AppList;

static void applist_push(AppList*l,App a){
    if(l->n==l->cap){ l->cap=l->cap?l->cap*2:128; l->v=realloc(l->v,l->cap*sizeof*l->v); }
    l->v[l->n++]=a;
}
static int applist_has_id(AppList*l,const char*id){
    for(int i=0;i<l->n;i++) if(!strcmp(l->v[i].id,id)) return 1;
    return 0;
}
static char *strip_field_codes(const char*exec){
    char*out=malloc(strlen(exec)+1),*o=out;
    for(const char*p=exec;*p;p++){ if(*p=='%'){ if(p[1]=='%'){*o++='%';p++;} else if(p[1])p++; } else *o++=*p; }
    *o='\0';
    char*r=out,*w=out; int sp=0;
    for(;*r;r++){ if(*r==' '){ if(!sp)*w++=' '; sp=1; } else { *w++=*r; sp=0; } }
    *w='\0'; return out;
}
static int parse_desktop(const char*path,const char*id,App*out){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    char*name=NULL,*exec=NULL,*type=NULL,*icon=NULL;
    int nodisplay=0,hidden=0,terminal=0,in=0; char line[8192];
    while(fgets(line,sizeof line,f)){
        char*s=trim(line);
        if(*s=='['){ if(in)break; in=!strcmp(s,"[Desktop Entry]"); continue; }
        if(!in||*s=='#') continue;
        char*eq=strchr(s,'='); if(!eq) continue;
        *eq='\0'; char*k=trim(s),*v=trim(eq+1);
        if(!name&&!strcmp(k,"Name"))name=xstrdup(v);
        else if(!exec&&!strcmp(k,"Exec"))exec=xstrdup(v);
        else if(!type&&!strcmp(k,"Type"))type=xstrdup(v);
        else if(!icon&&!strcmp(k,"Icon"))icon=xstrdup(v);
        else if(!strcmp(k,"NoDisplay"))nodisplay=!strcasecmp(v,"true");
        else if(!strcmp(k,"Hidden"))hidden=!strcasecmp(v,"true");
        else if(!strcmp(k,"Terminal"))terminal=!strcasecmp(v,"true");
    }
    fclose(f);
    int ok=1;
    if(!name||!exec) ok=0;
    if(type&&strcmp(type,"Application")) ok=0;
    if(nodisplay||hidden) ok=0;
    if(ok){
        out->id=xstrdup(id); out->name=name; name=NULL;
        out->exec=strip_field_codes(exec); out->icon=icon; icon=NULL;
        out->terminal=terminal; memset(&out->tex,0,sizeof out->tex); out->icon_tried=0;
    }
    free(name);free(exec);free(type);free(icon);
    return ok;
}
static void scan_dir(AppList*l,const char*dir){
    DIR*d=opendir(dir); if(!d) return;
    struct dirent*e;
    while((e=readdir(d))){
        const char*nm=e->d_name; size_t len=strlen(nm);
        if(len<9||strcmp(nm+len-8,".desktop")) continue;
        char id[256]; snprintf(id,sizeof id,"%.*s",(int)(len-8),nm);
        if(applist_has_id(l,id)) continue;
        char path[PATH_MAX]; snprintf(path,sizeof path,"%s/%s",dir,nm);
        App a; if(parse_desktop(path,id,&a)) applist_push(l,a);
    }
    closedir(d);
}
static void collect_default_dirs(char out[][PATH_MAX],int*n,int max){
    const char*home=getenv("HOME"),*xdh=getenv("XDG_DATA_HOME"),*xdd=getenv("XDG_DATA_DIRS");
    char buf[PATH_MAX];
    if(xdh&&*xdh) snprintf(buf,sizeof buf,"%s/applications",xdh);
    else snprintf(buf,sizeof buf,"%s/.local/share/applications",home?home:".");
    if(*n<max) snprintf(out[(*n)++],PATH_MAX,"%s",buf);
    const char*dirs=(xdd&&*xdd)?xdd:"/usr/local/share:/usr/share";
    char*copy=xstrdup(dirs),*tok=strtok(copy,":");
    while(tok&&*n<max){ snprintf(out[(*n)++],PATH_MAX,"%s/applications",tok); tok=strtok(NULL,":"); }
    free(copy);
    glob_t g;
    if(glob("/opt/*/share/applications",0,NULL,&g)==0)
        for(size_t i=0;i<g.gl_pathc&&*n<max;i++) snprintf(out[(*n)++],PATH_MAX,"%s",g.gl_pathv[i]);
    globfree(&g);
}
static int cmp_name(const void*a,const void*b){ return strcasecmp(((const App*)a)->name,((const App*)b)->name); }

static void apply_config_order(AppList*l,Config*c){
    if(*c->exclude){
        char*copy=xstrdup(c->exclude),*tok=strtok(copy,",");
        while(tok){ char*t=trim(tok);
            for(int i=0;i<l->n;i++)
                if(!strcasecmp(l->v[i].id,t)||!strcasecmp(l->v[i].name,t)){
                    free(l->v[i].id);free(l->v[i].name);free(l->v[i].exec);free(l->v[i].icon);
                    l->v[i]=l->v[--l->n]; i--;
                }
            tok=strtok(NULL,",");
        }
        free(copy);
    }
    if(*c->include){
        AppList kept={0};
        char*copy=xstrdup(c->include),*tok=strtok(copy,",");
        while(tok){ char*t=trim(tok);
            for(int i=0;i<l->n;i++)
                if(l->v[i].id&&(!strcasecmp(l->v[i].id,t)||!strcasecmp(l->v[i].name,t))){
                    applist_push(&kept,l->v[i]); l->v[i].id=NULL; break;
                }
            tok=strtok(NULL,",");
        }
        free(copy);
        for(int i=0;i<l->n;i++)
            if(l->v[i].id){ free(l->v[i].id);free(l->v[i].name);free(l->v[i].exec);free(l->v[i].icon); }
        free(l->v); *l=kept; return;
    }
    if(!strcmp(c->sort,"alpha")) qsort(l->v,l->n,sizeof*l->v,cmp_name);
}
static void apply_recency(AppList*l,Config*c){
    if(*c->include) return;
    if(strcmp(c->sort,"recent")) return;
    FILE*f=fopen(c->history,"r");
    qsort(l->v,l->n,sizeof*l->v,cmp_name);
    if(!f) return;
    App*ord=malloc(l->n*sizeof*ord); int on=0; char*used=calloc(l->n,1); char line[512];
    while(fgets(line,sizeof line,f)){ char*id=trim(line); if(!*id)continue;
        for(int i=0;i<l->n;i++) if(!used[i]&&!strcmp(l->v[i].id,id)){ l->v[i].recent=1; ord[on++]=l->v[i]; used[i]=1; break; } }
    fclose(f);
    for(int i=0;i<l->n;i++) if(!used[i]){ l->v[i].recent=0; ord[on++]=l->v[i]; }
    memcpy(l->v,ord,l->n*sizeof*l->v); free(ord); free(used);
}

/* Search ordering: split matches into "direct" (query starts a word in the name)
   and "close" (query only appears mid-word), then lay them out so the best match
   sits at the TOP, direct matches fan to the RIGHT, close matches to the LEFT.
   Fills filt[] and picks off/selslot so the best is centered (at the top). */
static int is_sep(char c){ return c==' '||c=='-'||c=='_'||c=='.'||c=='/'||c==':'; }
/* match quality: 0 exact, 1 whole-name prefix, 2 word-start, 3 mid-word, -1 none */
static int match_tier(const char*name,const char*q,size_t ql){
    if(!strcasecmp(name,q)) return 0;
    if(!strncasecmp(name,q,ql)) return 1;
    int ws=1;
    for(const char*p=name;*p;p++){
        if(ws && !strncasecmp(p,q,ql)) return 2;
        ws=is_sep(*p);
    }
    return contains_ci(name,q)?3:-1;
}
typedef struct { int idx,tier,len,rb; } Match;
static int match_cmp(const void*a,const void*b){
    const Match*x=a,*y=b;
    if(x->rb  !=y->rb  ) return x->rb  -y->rb;          /* recently-used first (if enabled) */
    if(x->tier!=y->tier) return x->tier-y->tier;       /* better tier first */
    if(x->len !=y->len ) return x->len -y->len;         /* shorter (closer) first */
    return x->idx-y->idx;                               /* then recency order */
}
static int build_filter(AppList*apps,const char*query,int*filt,int slots,int recent_first,int center_first,int*p_off,int*p_selslot){
    int fn=0;
    if(!query[0]){
        /* No query: the same shape a search gives, with recency standing in
           for the ranking. The one you used last sits at the top, the next
           few to its right, and the rest carry on round to its left — so the
           further from the top an app is, the longer since you opened it. */
        for(int i=0;i<apps->n;i++) filt[fn++]=i;
        if(fn<1){ *p_off=0; *p_selslot=0; return 0; }
        if(!center_first){ *p_off=0; *p_selslot=0; return fn; }

        /* Distance from the top is how long ago you used it: second to the
           right, third to the left, fourth to the right, and so on. The list
           is already history first and then everything else by name, so once
           the history runs out the alphabet simply carries on outwards. When
           one side fills up the rest goes on the other. */
        int c=(fn-1)/2, rank=1;
        int *tmp=malloc((size_t)fn*sizeof(int));
        if(!tmp){ *p_off=0; *p_selslot=0; return fn; }
        tmp[c]=filt[0];
        for(int r=c+1, l=c-1; rank<fn; ){
            if(r<fn  && rank<fn) tmp[r++]=filt[rank++];
            if(l>=0  && rank<fn) tmp[l--]=filt[rank++];
            if(r>=fn && l<0) break;
        }
        memcpy(filt,tmp,(size_t)fn*sizeof(int));
        free(tmp);

        int vis = fn<slots?fn:slots; if(vis<1)vis=1;
        if(vis>=4 && (vis&1)==0) vis--;                 /* odd: dead centre */
        int maxoff = fn>vis?fn-vis:0;
        int off = c-(vis-1)/2; if(off<0)off=0; if(off>maxoff)off=maxoff;
        int ss = c-off; if(ss<0)ss=0; if(ss>vis-1)ss=vis-1;
        *p_off=off; *p_selslot=ss;
        return fn;
    }
    size_t ql=strlen(query);
    Match *m=malloc(apps->n*sizeof(Match)); int nm=0;
    if(m) for(int i=0;i<apps->n;i++){
        const char*nm_s=apps->v[i].name, *id_s=apps->v[i].id;
        int tn=match_tier(nm_s,query,ql);                      /* match on the name ... */
        int ti=id_s?match_tier(id_s,query,ql):-1;              /* ...or the id */
        int t = tn<0?ti : (ti<0?tn : (tn<ti?tn:ti));           /* best of the two */
        if(t<0) continue;
        int mlen=1<<30;                                        /* shortest string that hit that tier */
        if(tn==t){ int l=(int)strlen(nm_s); if(l<mlen)mlen=l; }
        if(ti==t){ int l=(int)strlen(id_s); if(l<mlen)mlen=l; }
        m[nm].idx=i; m[nm].tier=t; m[nm].len=mlen;
        m[nm].rb=(recent_first && apps->v[i].recent)?0:1;      /* recently-used matches win */
        nm++;
    }
    if(nm>1) qsort(m,nm,sizeof(Match),match_cmp);       /* best-ranked first */
    int *dir=malloc(nm*sizeof(int)+1), *clo=malloc(nm*sizeof(int)+1);
    int nd=0,nc=0;
    if(dir&&clo) for(int j=0;j<nm;j++){                 /* keep ranked order within each group */
        if(m[j].tier<3) dir[nd++]=m[j].idx; else clo[nc++]=m[j].idx;
    }
    free(m);
    fn=nd+nc;
    if(fn<=0){ free(dir);free(clo); *p_off=0;*p_selslot=0; return 0; }
    int best,*right,nr,*left,nl;
    if(nd>0){ best=dir[0]; right=dir+1; nr=nd-1; left=clo;   nl=nc;   }
    else    { best=clo[0]; right=clo;   nr=0;    left=clo+1; nl=nc-1; }
    int c=(fn-1)/2;                                     /* best sits at the array centre */
    filt[c]=best;
    int rp=0,lp=0;
    for(int ri=c+1; ri<fn; ri++){                      /* right of centre: direct, then overflow */
        if(rp<nr) filt[ri]=right[rp++]; else if(lp<nl) filt[ri]=left[lp++];
    }
    for(int li=c-1; li>=0; li--){                      /* left of centre: close, then overflow */
        if(lp<nl) filt[li]=left[lp++]; else if(rp<nr) filt[li]=right[rp++];
    }
    free(dir); free(clo);
    int vis = fn<slots?fn:slots; if(vis<1)vis=1;
    if(vis>=4 && (vis&1)==0) vis--;                     /* odd -> best lands at the exact top */
    int maxoff = fn>vis?fn-vis:0;
    int off = c-(vis-1)/2; if(off<0)off=0; if(off>maxoff)off=maxoff;
    int ss = c-off; if(ss<0)ss=0; if(ss>vis-1)ss=vis-1;
    *p_off=off; *p_selslot=ss;
    return fn;
}

/* ------------------------------------------------------------------ */
/* icons                                                               */
/* ------------------------------------------------------------------ */
static void resolve_icon(const char*name,char*out,size_t n){
    out[0]='\0'; if(!name||!*name) return;
    if(strchr(name,'/')){ if(file_exists(name)) snprintf(out,n,"%s",name); return; }
    const char*home=getenv("HOME"),*xdh=getenv("XDG_DATA_HOME");
    char bases[8][PATH_MAX]; int nb=0;
    if(xdh&&*xdh) snprintf(bases[nb++],PATH_MAX,"%s/icons",xdh);
    else if(home) snprintf(bases[nb++],PATH_MAX,"%s/.local/share/icons",home);
    if(home) snprintf(bases[nb++],PATH_MAX,"%s/.icons",home);
    snprintf(bases[nb++],PATH_MAX,"/usr/local/share/icons");
    snprintf(bases[nb++],PATH_MAX,"/usr/share/icons");
    const char*themes[]={"hicolor","Adwaita","gnome","breeze","Papirus","Humanity",NULL};
    const char*sizes[]={"48x48","64x64","32x32","128x128","256x256","96x96","24x24","scalable",NULL};
    const char*cats[]={"apps","categories","devices","places","status","mimetypes","actions",NULL};
    const char*exts[]={"png","svg","jpg","bmp",NULL};
    char p[PATH_MAX];
    /* icons dropped directly in an icons dir (not a theme subdir), e.g.
       ~/.local/share/icons/duckduckgo.svg — check these first */
    for(int b=0;b<nb;b++)for(int e=0;exts[e];e++){
        snprintf(p,sizeof p,"%.3500s/%.400s.%s",bases[b],name,exts[e]);
        if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
    }
    for(int b=0;b<nb;b++)for(int t=0;themes[t];t++)for(int s=0;sizes[s];s++)
        for(int ca=0;cats[ca];ca++)for(int e=0;exts[e];e++){
            snprintf(p,sizeof p,"%.3500s/%s/%s/%s/%.400s.%s",bases[b],themes[t],sizes[s],cats[ca],name,exts[e]);
            if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
        }
    const char*pm[]={"/usr/share/pixmaps","/usr/local/share/pixmaps",NULL};
    for(int i=0;pm[i];i++)for(int e=0;exts[e];e++){
        snprintf(p,sizeof p,"%s/%.400s.%s",pm[i],name,exts[e]);
        if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
    }
}
/* An icon is now just its pixels: straight RGBA, which is what both stb_image
   and nanosvg hand back and what sw_blit takes. */
static Image load_icon_img(const char*path){
    Image im={0};
    int w,h,ch; unsigned char*data=stbi_load(path,&w,&h,&ch,4);
    if(!data) return im;
    im.rgba=data; im.w=w; im.h=h; im.stride=w*4;
    return im;
}
static int ends_with_ci(const char*s,const char*suf){
    size_t ls=strlen(s),lf=strlen(suf);
    return ls>=lf && strcasecmp(s+ls-lf,suf)==0;
}
/* Rasterise an SVG icon to ~px pixels, aspect preserved and nothing cut off.
   Scaling by the declared width/height alone crops any icon whose drawing runs
   outside it — a viewBox that does not cover the art, or none at all — because
   the rasteriser simply does not draw past the box it is given. The box used
   here is the declared canvas and the shapes' own bounds together: an icon
   with deliberate padding keeps it, one that overflows is scaled down instead
   of losing an edge. */
static Image load_svg_img(const char*path,int px){
    Image out={0};
    if(px<16)px=16; if(px>512)px=512;
    NSVGimage*img=nsvgParseFromFile(path,"px",96.0f);
    if(!img) return out;
    float x0=0,y0=0,x1=img->width,y1=img->height;
    if(x1<=0||y1<=0){ x0=y0=1e30f; x1=y1=-1e30f; }   /* no usable canvas */
    for(NSVGshape*s=img->shapes;s;s=s->next){
        if(!(s->flags&NSVG_FLAGS_VISIBLE)) continue;
        float m=s->strokeWidth*0.5f;                 /* a stroke sits astride */
        if(s->bounds[0]-m<x0) x0=s->bounds[0]-m;
        if(s->bounds[1]-m<y0) y0=s->bounds[1]-m;
        if(s->bounds[2]+m>x1) x1=s->bounds[2]+m;
        if(s->bounds[3]+m>y1) y1=s->bounds[3]+m;
    }
    float bw=x1-x0, bh=y1-y0;
    if(!(bw>0.0f)||!(bh>0.0f)){ nsvgDelete(img); return out; }
    float sc=(float)px/(bw>bh?bw:bh);
    int ow=(int)(bw*sc+0.5f), oh=(int)(bh*sc+0.5f);
    if(ow<1)ow=1; if(oh<1)oh=1;
    unsigned char*pix=calloc((size_t)ow*oh,4);
    NSVGrasterizer*r=nsvgCreateRasterizer();
    if(!pix||!r){ free(pix); if(r)nsvgDeleteRasterizer(r); nsvgDelete(img); return out; }
    nsvgRasterize(r,img,-x0*sc,-y0*sc,sc,pix,ow,oh,ow*4);
    nsvgDeleteRasterizer(r); nsvgDelete(img);
    out.rgba=pix; out.w=ow; out.h=oh; out.stride=ow*4;
    return out;
}
static Image app_icon(App*a,Config*c){
    Image none={0};
    if(!c->icons) return none;
    if(!a->icon_tried){ a->icon_tried=1;
        char path[PATH_MAX]; resolve_icon(a->icon,path,sizeof path);
        if(*path){
            if(ends_with_ci(path,".svg")){
                int px=(int)(c->icon_px*c->ui_scale*2.0f); if(px<48)px=48; if(px>256)px=256;
                a->tex=load_svg_img(path,px);
            } else {
                a->tex=load_icon_img(path);
            }
        }
    }
    return a->tex;
}

/* ------------------------------------------------------------------ */
/* launching + history                                                 */
/* ------------------------------------------------------------------ */
static void mkparents(const char*path){
    char tmp[PATH_MAX]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char*p=tmp+1;*p;p++) if(*p=='/'){ *p='\0'; mkdir(tmp,0755); *p='/'; }
}
static void history_prepend(Config*c,const char*id){
    mkparents(c->history);
    char*lines[256]; int ln=0; FILE*f=fopen(c->history,"r");
    if(f){ char b[512]; while(ln<255&&fgets(b,sizeof b,f)){ char*t=trim(b);
        if(!*t||!strcmp(t,id))continue; lines[ln++]=xstrdup(t); }
        fclose(f);
    }
    f=fopen(c->history,"w"); if(!f){ for(int i=0;i<ln;i++)free(lines[i]); return; }
    fprintf(f,"%s\n",id);
    for(int i=0;i<ln;i++){ fprintf(f,"%s\n",lines[i]); free(lines[i]); }
    fclose(f);
}
/* ------------------------------------------------------------------ */
/* workspaces — everything sway-shaped is asked of swov, so this file    */
/* needs no IPC, no JSON and no knowledge of the compositor.             */
/* ------------------------------------------------------------------ */
typedef struct {
    int  num;                 /* -1 for a workspace that only has a name */
    char name[64];            /* "3" or "3:code"                         */
    int  focused, exists;     /* exists = 0 -> a free number, a new one   */
} Wsp;

static Wsp  WSP[24];
static int  NWSP, WSP_TRIED;

static const char *wsp_label(const Wsp*w){
    const char*colon=strchr(w->name,':');
    return colon&&colon[1]?colon+1:"";
}

static int wsp_cmp(const void*A,const void*B){
    const Wsp*a=A,*b=B;
    if((a->num<0)!=(b->num<0)) return a->num<0?1:-1;   /* named-only ones last */
    if(a->num!=b->num) return a->num-b->num;
    return strcmp(a->name,b->name);
}

/* `swov --workspaces`, plus the free numbers 1..9 as places to make one */
static void wsp_load(Config*c){
    if(WSP_TRIED) return;
    WSP_TRIED=1; NWSP=0;

    char cmd[PATH_MAX+64];
    snprintf(cmd,sizeof cmd,"%s --workspaces 2>/dev/null",c->swov);
    FILE*f=popen(cmd,"r");
    if(!f) return;

    char line[256];
    while(NWSP<(int)(sizeof WSP/sizeof*WSP) && fgets(line,sizeof line,f)){
        char*num=strtok(line,"\t"); if(!num) continue;
        char*name=strtok(NULL,"\t"); if(!name) continue;
        strtok(NULL,"\t");                       /* output, not used here */
        char*flags=strtok(NULL,"\t\n");
        Wsp*w=&WSP[NWSP++];
        memset(w,0,sizeof *w);
        w->num=atoi(num);
        snprintf(w->name,sizeof w->name,"%s",name);
        w->exists=1;
        w->focused=flags&&contains_ci(flags,"focused");
    }
    pclose(f);
    if(NWSP==0) return;                          /* no swov, no dropping   */

    for(int n=1;n<=9 && NWSP<(int)(sizeof WSP/sizeof*WSP);n++){
        int taken=0;
        for(int i=0;i<NWSP;i++) if(WSP[i].num==n){ taken=1; break; }
        if(taken) continue;
        Wsp*w=&WSP[NWSP++];
        memset(w,0,sizeof *w);
        w->num=n; snprintf(w->name,sizeof w->name,"%d",n);
    }
    qsort(WSP,NWSP,sizeof*WSP,wsp_cmp);
}

/* ------------------------------------------------------------------ */
/* the overview behind the wheel                                        */
/*                                                                      */
/* `swov --backdrop` draws the workspaces underneath us and takes no     */
/* input of its own: we send it the pointer, it sends back the workspace */
/* under it. Two pipes, one line each way, no shared code.               */
/* ------------------------------------------------------------------ */
static int   OV_IN=-1, OV_OUT=-1;     /* our end of its stdin / stdout */
static pid_t OV_PID=-1;
static int   OV_READY;                /* its window is up              */
static char  OV_TARGET[64];           /* the workspace under the cursor */
static int   OV_HERE;                 /* ...and it is the one we are on  */
static char  OV_BESIDE[32];           /* con_id of the window under it    */
static char  OV_EDGE[16];             /* which side of it: left/right/... */
static int   OV_LOG;                  /* print the conversation to stderr */

static void ov_send(const char*fmt,...){
    if(OV_IN<0) return;
    char line[128]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof line-1,fmt,ap); va_end(ap);
    if(n<0) return;
    line[n]='\n'; line[n+1]='\0';
    if(OV_LOG) fprintf(stderr,"swas: -> %.*s\n",n,line);
    if(write(OV_IN,line,(size_t)n+1)<0){
        if(OV_LOG) fprintf(stderr,"swas: backdrop pipe closed\n");
        close(OV_IN); OV_IN=-1;
    }
}

/* Take over from a wheel that is already up.
 *
 * Two of these on top of each other is confusing and the second one grabs the
 * keyboard, so the binding should just open the one that is there. Same
 * approach as swbr: clear the way rather than race a pkill. */
static void replace_running(void){
    DIR*d=opendir("/proc"); if(!d) return;
    pid_t me=getpid(), victims[32]; int n=0;
    struct dirent*e;
    while((e=readdir(d))&&n<(int)(sizeof victims/sizeof*victims)){
        bool digits=e->d_name[0]!=0;
        for(const char*q=e->d_name;*q;q++) if(*q<'0'||*q>'9'){digits=false;break;}
        if(!digits) continue;
        pid_t pid=(pid_t)atoi(e->d_name);
        if(pid==me||pid<=1) continue;
        char path[64],comm[64]="";
        snprintf(path,sizeof path,"/proc/%d/comm",(int)pid);
        FILE*f=fopen(path,"r"); if(!f) continue;
        if(fgets(comm,sizeof comm,f)){ char*nl=strchr(comm,'\n'); if(nl)*nl=0; }
        fclose(f);
        if(strcmp(comm,APP_ID)) continue;
        struct stat st; snprintf(path,sizeof path,"/proc/%d",(int)pid);
        if(stat(path,&st)!=0||st.st_uid!=getuid()) continue;
        victims[n++]=pid;
    }
    closedir(d);
    for(int i=0;i<n;i++) kill(victims[i],SIGTERM);
    for(int w=0;w<40&&n;w++){
        usleep(50000);
        int left=0;
        for(int i=0;i<n;i++) if(kill(victims[i],0)==0) victims[left++]=victims[i];
        n=left;
    }
    for(int i=0;i<n;i++) kill(victims[i],SIGKILL);
}

static int OV_ONCE=0;   /* launched by an overview: close after one drop */

/* Started by an overview that is already up: it hands us two pipes and stays
   where it is, so the wheel appears in front of the overview instead of
   replacing it. One drag, one drop, and we are gone again. */
static int ov_adopt(void){
    const char*i=getenv("SWAS_OV_IN"), *o=getenv("SWAS_OV_OUT");
    if(!i||!*i||!o||!*o) return 0;
    int rd=atoi(i), wr=atoi(o);      /* what swov gave us: read, write */
    if(rd<=0||wr<=0) return 0;
    signal(SIGPIPE,SIG_IGN);
    OV_OUT=rd;                       /* we read its replies here */
    OV_IN =wr;                       /* and send it commands here */
    fcntl(OV_OUT,F_SETFL,O_NONBLOCK);
    OV_ONCE=1;
    return 1;
}

static uint64_t now_ms_(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000u + (uint64_t)(ts.tv_nsec/1000000);
}

static void ov_spawn(Config*c){
    OV_LOG=c->overview_debug;
    int to[2], from[2];
    if(pipe(to)!=0) return;
    if(pipe(from)!=0){ close(to[0]); close(to[1]); return; }

    signal(SIGPIPE,SIG_IGN);          /* it may be gone before we notice */
    pid_t p=fork();
    if(p<0){ close(to[0]);close(to[1]);close(from[0]);close(from[1]); return; }
    if(p==0){
        dup2(to[0],0); dup2(from[1],1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        execlp(c->swov,c->swov,c->overview_debug?"--backdrop-debug":"--backdrop",
               (char*)NULL);
        fprintf(stderr,"swas: cannot run '%s' (%s) — no overview\n",
                c->swov,strerror(errno));
        _exit(127);
    }
    close(to[0]); close(from[1]);
    OV_PID=p; OV_IN=to[1]; OV_OUT=from[0];
    fcntl(OV_OUT,F_SETFL,O_NONBLOCK);
}

static void ov_poll(void);

/* The overview is only wanted once an app is actually being dragged
   somewhere, so it is started then rather than at launch: opening the wheel
   opens the wheel and nothing else. swov is up in about fifteen
   milliseconds, so the wait here is normally over before the drag has moved
   far; if it is not, the drag falls back to the plain workspace tiles. */
static int OV_SPAWNED;
static int ov_bring_up(Config *c, int wait_ms)
{
    uint64_t t0;
    if(OV_READY) return 1;
    if(!c->overview) return 0;
    if(!OV_SPAWNED){ OV_SPAWNED=1; ov_spawn(c); }
    if(OV_OUT<0) return 0;
    t0=now_ms_();
    while(!OV_READY && (int64_t)(now_ms_()-t0) < wait_ms){
        sw_dispatch(4);
        ov_poll();
    }
    return OV_READY;
}

/* whatever it has said since the last frame */
static void ov_poll(void){
    if(OV_OUT<0) return;
    static char buf[512]; static int len;

    for(;;){
        ssize_t n=read(OV_OUT,buf+len,sizeof buf-1-(size_t)len);
        if(n<=0) break;
        len+=(int)n; buf[len]='\0';

        char*start=buf,*nl;
        while((nl=strchr(start,'\n'))!=NULL){
            *nl='\0';
            if(OV_LOG) fprintf(stderr,"swas: <- %s\n",start);
            if(!strcmp(start,"ready")){
                /* Nothing to raise: the wheel is a layer surface above the
                   backdrop's, so it is already in front with the keyboard. */
                OV_READY=1;
            } else if(!strncmp(start,"target ",7)){
                /* "target 3", "target 3 current", "target 3 beside 42 left" */
                char buf[160]; snprintf(buf,sizeof buf,"%s",start+7);
                OV_HERE=0; OV_BESIDE[0]='\0'; OV_EDGE[0]='\0';
                char*tok=strtok(buf," ");
                snprintf(OV_TARGET,sizeof OV_TARGET,"%s",
                         (tok&&strcmp(tok,"none"))?tok:"");
                while((tok=strtok(NULL," "))!=NULL){
                    if(!strcmp(tok,"current")) OV_HERE=1;
                    else if(!strcmp(tok,"beside")){
                        char*id=strtok(NULL," "), *ed=strtok(NULL," ");
                        if(id&&ed){ snprintf(OV_BESIDE,sizeof OV_BESIDE,"%s",id);
                                    snprintf(OV_EDGE,sizeof OV_EDGE,"%s",ed); }
                    }
                    else if(!strcmp(tok,"outer")){   /* beside the whole lot */
                        char*ed=strtok(NULL," ");
                        if(ed){ OV_BESIDE[0]='\0';
                                snprintf(OV_EDGE,sizeof OV_EDGE,"%s",ed); }
                    }
                }
            }
            start=nl+1;
        }
        len=(int)strlen(start);
        memmove(buf,start,(size_t)len+1);
    }
}

static void ov_stop(void){
    if(OV_IN>=0){ ov_send("quit"); close(OV_IN); OV_IN=-1; }
    if(OV_OUT>=0){ close(OV_OUT); OV_OUT=-1; }
}

/* Hand the launched process to swov, which waits for its window and moves it.
   sway cannot run something "on workspace N", so this is the way there. */
static void adopt_to(Config*c,pid_t pid,const char*target,
                     const char*beside,const char*edge){
    if(pid<=0||!target||!*target) return;
    char spid[32];
    snprintf(spid,sizeof spid,"%d",(int)pid);

    pid_t p=fork();
    if(p<0) return;
    if(p==0){
        int fd=open("/dev/null",O_RDWR);
        if(fd>=0){ dup2(fd,0); dup2(fd,1); dup2(fd,2); if(fd>2) close(fd); }
        const char*av[14]; int n=0;
        av[n++]=c->swov; av[n++]="--adopt"; av[n++]=spid; av[n++]=target;
        /* Assigning the app to a workspace before it opens means it maps on
           one that is not on screen. A toolkit that picks its scale from the
           output it lands on has no output to look at, takes 1, and comes out
           the wrong size on a scaled screen. Without the rule it opens here,
           at the right scale, and is moved after. */
        if(!c->drop_assign) av[n++]="--no-assign";
        if(edge&&*edge){
            if(beside&&*beside){ av[n++]="--beside"; av[n++]=beside; }
            av[n++]="--edge"; av[n++]=edge;
        }
        if(c->drop_focus)      av[n++]="--adopt-focus";
        if(c->overview_debug)  av[n++]="--adopt-debug";
        av[n]=NULL;
        execvp(c->swov,(char*const*)av);
        _exit(127);
    }
    waitpid(p,NULL,0);        /* swov backgrounds itself, so this is instant */
}

static void wsp_adopt(Config*c,pid_t pid,const Wsp*w){
    char target[80];
    if(w->num>=0) snprintf(target,sizeof target,"%d",w->num);
    else          snprintf(target,sizeof target,"%s",w->name);
    adopt_to(c,pid,target,NULL,NULL);
}

/* Something to say when an app does not appear.
 *
 * A launcher that silently does nothing is the worst kind: you press, nothing
 * happens, and there is no way to tell a typo in a .desktop file from a slow
 * program. The pid is watched for a moment after launching, and if it is
 * gone by then the app never really started. */
static char  ERR_TEXT[192];
static double ERR_UNTIL;
static pid_t  WATCH_PID;
static double WATCH_AT;
static char   WATCH_NAME[64];

static double now_secs_(void){ return (double)now_ms_()/1000.0; }

static void err_say(const char*fmt,...){
    va_list ap; va_start(ap,fmt);
    vsnprintf(ERR_TEXT,sizeof ERR_TEXT,fmt,ap);
    va_end(ap);
    ERR_UNTIL = now_secs_() + 6.0;
}

static void watch_tick(void){
    if(!WATCH_PID) return;
    if(now_secs_() - WATCH_AT < 1.5) return;

    char path[64];
    snprintf(path,sizeof path,"/proc/%d",(int)WATCH_PID);
    if(access(path,F_OK)!=0)
        err_say("%s did not start", WATCH_NAME);
    WATCH_PID=0;
}

static pid_t launch(App*a,Config*c){
    char cmd[8192];
    if(!strcmp(c->launcher,"gtk-launch")) snprintf(cmd,sizeof cmd,"gtk-launch %s.desktop",a->id);
    else if(a->terminal) snprintf(cmd,sizeof cmd,"%s -e %s",c->terminal,a->exec);
    else snprintf(cmd,sizeof cmd,"%s",a->exec);
    history_prepend(c,a->id);

    /* Launch fully detached from whatever started us. Without this, the child
       inherits our stdin/stdout/stderr and controlling terminal, so a terminal
       app (or anything that touches the tty) corrupts the shell we were run
       from. We: new session (setsid) so there's no controlling tty, a second
       fork so it can never reacquire one and gets reparented to init, and
       std fds pointed at /dev/null. */
    /* The grandchild is the one that becomes the app, and its pid is what a
       drop needs, so it reports itself back through a pipe. */
    int pfd[2];
    if(pipe(pfd)!=0){ pfd[0]=pfd[1]=-1; }

    pid_t pid=fork();
    if(pid<0){ fprintf(stderr,"swas: fork failed\n");
               if(pfd[0]>=0){ close(pfd[0]); close(pfd[1]); } return -1; }
    if(pid==0){
        if(pfd[0]>=0) close(pfd[0]);
        setsid();
        pid_t p2=fork();
        if(p2>0) _exit(0);
        if(pfd[1]>=0){
            pid_t me=getpid();
            if(write(pfd[1],&me,sizeof me)<0){}
            /* Kept open across the exec, so that if the exec fails this end
               closes and the reader learns the app never started. On success
               it is closed by the exec itself. */
            fcntl(pfd[1],F_SETFD,FD_CLOEXEC);
        }
        int fd=open("/dev/null",O_RDWR);
        if(fd>=0){ dup2(fd,0); dup2(fd,1); dup2(fd,2); if(fd>2) close(fd); }
        const char*home=getenv("HOME"); if(home){ if(chdir(home)!=0){} }
        execl("/bin/sh","sh","-c",cmd,(char*)NULL);
        _exit(127);
    }
    waitpid(pid,NULL,0);   /* reap the intermediate child; grandchild -> init */

    pid_t app=-1;
    if(pfd[1]>=0) close(pfd[1]);
    if(pfd[0]>=0){
        if(read(pfd[0],&app,sizeof app)!=(ssize_t)sizeof app) app=-1;
        close(pfd[0]);
    }

    if(app<=0){
        err_say("could not start %s", (a->name&&a->name[0])?a->name:a->id);
    } else {
        WATCH_PID = app;
        WATCH_AT  = now_secs_();
        snprintf(WATCH_NAME,sizeof WATCH_NAME,"%s", (a->name&&a->name[0])?a->name:a->id);
    }
    return app;
}

/* ------------------------------------------------------------------ */
/* geometry primitives                                                 */
/* ------------------------------------------------------------------ */
/* The shapes the wheel is made of: thin wrappers over sw_draw, antialiased
   in software. The signatures are the ones the drawing code below expects. */
static void fill_circle(Canvas*cv,float cx,float cy,float rad,Col c){
    sw_fill_circle(cv,cx,cy,rad,c);
}
/* a thin outline circle: the "let go here" hint around the parked wheel */
static void ring(Canvas*cv,float cx,float cy,float rad,float thick,Col c){
    sw_stroke_round(cv,cx-rad,cy-rad,rad*2,rad*2,rad,rad,rad,rad,thick,c);
}
static void fill_sector(Canvas*cv,float cx,float cy,float r0,float r1,float a0,float a1,Col c){
    sw_fill_sector(cv,cx,cy,r0,r1,a0,a1,c);
}
static void fill_round_rect(Canvas*cv,float x,float y,float w,float h,float rad,Col c){
    sw_fill_round(cv,x,y,w,h,rad,rad,rad,rad,c);
}
static void stroke_rect(Canvas*cv,float x,float y,float w,float h,float t,Col c){
    sw_stroke_round(cv,x,y,w,h,0,0,0,0,t,c);
}
static void fill_tri(Canvas*cv,float x0,float y0,float x1,float y1,float x2,float y2,Col c){
    sw_fill_tri(cv,x0,y0,x1,y1,x2,y2,c);
}
/* stacked arrowheads pointing dir (+1 right / -1 left); count grows with speed */
static void draw_chevrons(Canvas*cv,float x,float y,int dir,float s,int count,Col c){
    if(count<1)count=1;
    float gap=s*0.72f, total=(count-1)*gap, start=x-dir*total/2;
    for(int i=0;i<count;i++){
        float ax=start+dir*i*gap;
        fill_tri(cv, ax+dir*s*0.5f, y,
                     ax-dir*s*0.5f, y-s*0.62f,
                     ax-dir*s*0.5f, y+s*0.62f, c);
    }
}
static float norm_ang(float a){ while(a>M_PI)a-=2*M_PI; while(a<=-M_PI)a+=2*M_PI; return a; }

/* is the cursor on a paging ICON? (used only for the startup arming) */
static int on_icon(float mx,float my,float cx,float cy,float rl,float hitr){
    float lx=cx+rl*cosf(ICON_AL), ly=cy+rl*sinf(ICON_AL);
    float rx=cx+rl*cosf(ICON_AR), ry=cy+rl*sinf(ICON_AR);
    float h2=hitr*hitr;
    float a=mx-lx,b=my-ly, c=mx-rx,d=my-ry;
    return (a*a+b*b<=h2) || (c*c+d*d<=h2);
}

/* ------------------------------------------------------------------ */
static void dump_config(void){
    fputs(
"# swas config  —  ~/.config/swas/config\n"
"# One key=value per line. '#' starts a comment. Blank lines are ignored.\n"
"# Every key here also works on the command line (key=value or --key=value);\n"
"# command-line values win over the file. See `swas --help` for the full list.\n"
"# The values below are the built-in defaults, so this file changes nothing\n"
"# until you edit it.\n"
"\n"
"# --- window ---\n"
"width=900\n"
"height=900\n"
"fullscreen=1     # cover the screen as a borderless overlay (0 = 900x900 window)\n"
"close_on_focus_loss=0   # 1 = quit when the window loses focus (dmenu-style)\n"
"\n"
"# --- ring layout & paging ---\n"
"slots=11          # wide, easy-to-hit slots across the TOP arc\n"
"arc=240           # degrees of the ring used for apps (rest = paging zone)\n"
"radius=0.44       # wheel size, as a fraction of the shorter screen side\n"
"y_offset=0.10     # nudge the wheel down (apps sit up top, so this centers it)\n"
"page_ms=110       # base ms per paged step at the SLOW end. Bigger = calmer\n"
"                  # start. Paging eases in: slow where you enter the bottom\n"
"                  # zone, fast toward straight-down.\n"
"\n"
"# --- rendering ---\n"
"ssaa=2            # ignored: every shape is antialiased as it is drawn. alias: aa\n"
"animate=1         # crossfade app names/icons when results change\n"
"anim_ms=90        # its duration (0 or animate=0 to turn it off)\n"
"recent_first=1    # bias the best match toward your recently-used apps\n"
"icons=1           # show .desktop icons if found (PNG/SVG/JPG/BMP)\n"
"icon_px=46        # icon size (before ui_scale)\n"
"\n"
"# --- text ---\n"
"ui_scale=1.0      # master text size. 1.3 = bigger, 0.85 = smaller.\n"
"                  # aliases: font_scale, text_scale\n"
"label_px=24       # app labels on the wheel   (all four are multiplied by\n"
"title_px=25       # selected app name (center)  ui_scale, so tweak one or all)\n"
"search_px=20      # the text you type (search box)\n"
"count_px=20       # the \"3 / 42\" counter\n"
"# By default swas uses your desktop's configured font (via fontconfig).\n"
"# Set a .ttf path or a family name to override, e.g. font=JetBrains Mono\n"
"# font=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\n"
"font_px=50        # glyph atlas height; the sharpness ceiling for big text\n"
"\n"
"# --- dropping an app onto a workspace ---\n"
"# Drag an app out of the wheel and let go over a workspace: it starts there,\n"
"# and the wheel stays open. Needs swov on PATH, which is asked for the\n"
"# workspace list and does the moving.\n"
"drop=1           # 0 = off, the wheel then only launches\n"
"drop_size=132    # how wide the wheel is while you drag, in pixels. It is\n"
"                 # scaled, not redrawn, so it looks the same, just small.\n"
"                 # 0 = use drop_shrink as a fraction of the screen instead\n"
"drop_shrink=0.55 # that fraction, when drop_size is 0\n"
"drop_corner=center  # where it sits then: center, top-left, top-right,\n"
"                 # bottom-left, bottom-right. Letting go over the wheel\n"
"                 # cancels wherever it is\n"
"drop_bg=0d1117d9 # a disc behind the wheel while it is small, so it does\n"
"                 # not get lost over the overview. Alpha 0 for none\n"
"drop_ms=130      # how long that takes\n"
"drop_px=10       # movement before a press turns into a drag\n"
"drop_assign=1    # assign the app to its workspace before it opens. 0 =\n"
"                 # let it open on the workspace you are on and move it\n"
"                 # after, which is what a toolkit that reads its scale from\n"
"                 # the output it lands on needs\n"
"drop_focus=0     # 1 = also switch to that workspace\n"
"drop_close_here=0  # dropping on the workspace you are on keeps the wheel\n"
"                 # open too; 1 = close, so the new window is not behind it\n"
"# swov=swov      # the binary to ask (a path works too)\n"
"\n"
"# --- launching / ordering ---\n"
"# swas logs every app you launch to the history file below and shows the\n"
"# most-recently-opened ones first. This is the default (sort=recent).\n"
"center_first=1   # the app you used last at the top of the arc, second to\n"
"                 # its right, third to its left, and so on outwards; then\n"
"                 # the rest alphabetically. 0 = fill from the left\n"
"sort=recent       # recent = most-recently-opened first (DEFAULT) | alpha = A-Z\n"
"launcher=sh       # sh = run Exec= ; gtk-launch = launch by desktop id\n"
"terminal=xterm    # used for Terminal=true entries when launcher=sh\n"
"# history=~/.cache/swas/history   # the opened-apps log (auto-created)\n"
"\n"
"# which apps show / in what order (an id is the .desktop filename without\n"
"# the extension; run `swas --list` to see them):\n"
"# dirs=~/.local/share/applications:/usr/share/applications\n"
"# include=firefox,code,gimp     # show ONLY these ids, in this exact order\n"
"# exclude=htop,xterm            # hide these (matches id OR Name)\n"
"\n"
"# --- colors: #rrggbb or #rrggbbaa ---\n"
"# bg alpha < ff => transparent window background (needs a compositor)\n"
"bg=0d111700\n"
"ring=1e2733f2\n"
"ring2=26313ff2\n"
"hl=cb9b00ff        # highlighted slot\n"
"text=e8e8e8ff      # normal slot text\n"
"hltext=141414ff    # text on the highlighted slot\n"
"center=0d1117e6    # hub disc (keep some alpha so search text stays legible)\n"
"accent=89afc4ff    # typed query text + active paging chevrons\n"
"dim=5a6b7aff       # hints / counters / idle chevrons\n",
    stdout);
}

static void usage(const char*a0){
    printf(
"swas " SWAS_VERSION " (build " SWAS_BUILD ") — Sway App Selector, a radial launcher\n"
"\n"
"USAGE\n"
"  %s [options] [key=value ...]\n"
"\n"
"  Every config key below is also a command-line argument. Both forms work:\n"
"      swas slots=8 arc=220 icons=0\n"
"      swas --slots=8 --arc=220 --icons=0\n"
"  CLI values override the config file.\n"
"\n"
"OPTIONS\n"
"  -c, --config PATH   config file (default: $XDG_CONFIG_HOME/wheel/config)\n"
"      --dump-config   print a commented default config (redirect to save it):\n"
"                        swas --dump-config > ~/.config/swas/config\n"
"      --list          print discovered apps (with resolved icon) and exit\n"
"      --no-recent     rank matches purely by relevance, ignoring recent-app bias\n"
"  -d, --dmenu         read newline-separated items from stdin, print the chosen\n"
"                      one to stdout (a dmenu/bemenu/wofi-style picker). dmenu's\n"
"                      own flags (-l N, -p PROMPT, -i, ...) imply it and are\n"
"                      otherwise ignored, so MENU=swas works in dmenu scripts\n"
"  -h, --help          show this help and exit\n"
"  -v, --version       print the version and build id, and exit\n"
"      --replace       close a wheel that is already open, then start\n"
"\n"
"LAYOUT / INTERACTION\n"
"  slots=11            wide, easy-to-hit app slots across the top arc\n"
"  arc=240             degrees of the ring used for apps (rest = paging zone)\n"
"  radius=0.44         wheel size (fraction of the shorter screen side)\n"
"  y_offset=0.10       nudge the wheel down so it looks vertically centered\n"
"  page_ms=110         base ms per paged step; the farther left/right the\n"
"                      cursor sits in the bottom zone, the faster it pages\n"
"\n"
"ICONS  (PNG, SVG, JPG, BMP)\n"
"  icons=1             1 = show .desktop icons, 0 = labels only\n"
"  icon_px=46          icon draw size (before ui_scale)\n"
"\n"
"TEXT\n"
"  ui_scale=1.0        master text size (aliases: font_scale, text_scale)\n"
"  label_px=24         app labels on the wheel     ) each is multiplied\n"
"  title_px=25         selected app name (center)  ) by ui_scale; set any\n"
"  search_px=20        the text you type           ) one independently\n"
"  count_px=20         the \"3 / 42\" counter        )\n"
"  font=PATH|FAMILY    a .ttf path, or a fontconfig family like \"JetBrains Mono\".\n"
"                      Default: your desktop's configured font (via fc-match).\n"
"  font_px=50          atlas raster height; larger = crisper big text\n"
"\n"
"DROP ONTO A WORKSPACE\n"
"  Drag an app out of the wheel and drop it on a workspace: it starts there\n"
"  and the wheel stays open. Dropping it back on the wheel does nothing.\n"
"  Needs swov on PATH — it supplies the workspace list and moves the window.\n"
"  drop=1              0 = off\n"
"  drop_size=132       how wide the wheel is while dragging, in pixels;\n"
"                      0 falls back to drop_shrink as a fraction\n"
"  drop_corner=center  where it sits then: center, top-left, top-right,\n"
"                      bottom-left, bottom-right. Letting go over the wheel\n"
"                      cancels wherever it is\n"
"  drop_bg=0d1117d9    a disc behind it while it is small\n"
"  drop_ms=130 drop_px=10\n"
"  drop_focus=0        1 = also switch to that workspace\n"
"  swov=swov           the binary to ask\n"
"\n"
"RENDERING\n"
"  ssaa=2              ignored; every shape is antialiased as it is drawn (alias: aa)\n"
"  animate=1 anim_ms=90  crossfade names/icons when results change (0 = off)\n"
"  width=900 height=900 fullscreen=1  close_on_focus_loss=0\n"
"\n"
"SOURCES / ORDER\n"
"  An app's \"id\" is just its .desktop filename without the extension:\n"
"      /usr/share/applications/firefox.desktop      -> id \"firefox\"\n"
"      ~/.local/share/applications/spotify.desktop  -> id \"spotify\"\n"
"  Run  swas --list  to print every id next to its visible Name.\n"
"\n"
"  sort=recent                    recent (most-recently-opened first, the\n"
"                                 DEFAULT) | alpha (A-Z). Every launch is\n"
"                                 logged to the history file automatically.\n"
"  include=firefox,gimp,spotify   show ONLY these ids, in this exact order\n"
"  exclude=htop,xterm             hide these (matches the id OR the Name)\n"
"  dirs=DIR:DIR                   scan these instead of the default dirs, e.g.\n"
"      dirs=~/.local/share/applications:/usr/share/applications\n"
"  launcher=sh                    sh = run Exec= ; gtk-launch = launch by id\n"
"  terminal=xterm                 terminal for Terminal=true apps (launcher=sh)\n"
"  history=PATH                   recency file (default ~/.cache/wheel/history)\n"
"\n"
"EXAMPLES\n"
"  swas --list                          # discover ids\n"
"  swas include=firefox,code,gimp       # a curated wheel, in that order\n"
"  swas exclude=htop sort=alpha         # hide htop, list A-Z\n"
"  swas ui_scale=1.3 icon_px=56         # bigger text and icons\n"
"\n"
"COLORS  (#rrggbb or #rrggbbaa)\n"
"  bg ring ring2 hl text hltext center accent dim\n"
"  bg alpha < ff => transparent window background (needs a compositor)\n"
"\n"
"CONTROLS\n"
"  mouse over top arc      highlight a slot\n"
"  bottom-left / -right    page back / forward (farther out = faster)\n"
"  scroll wheel            scroll the list under the fixed highlight\n"
"  type                    filter by name        Backspace  edit filter\n"
"  Enter / left-click      launch                Esc        clear filter / quit\n"
"  Right-click             quit\n",
    a0);
}

/* Load the app/menu list (stdin for dmenu, else scan .desktop dirs). Kept
   separate so the window can be created first — see main(). */
static void load_apps(AppList*apps, Config*cfg, int dmenu){
    if(dmenu){                                     /* dmenu mode: items come from stdin */
        char line[8192];
        while(fgets(line,sizeof line,stdin)){
            size_t L=strlen(line);
            while(L && (line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
            if(!L) continue;               /* a blank line is a gap in a list,
                                              not an empty slice on the wheel */
            App a; memset(&a,0,sizeof a);
            a.id=xstrdup(line); a.name=xstrdup(line);
            applist_push(apps,a);                  /* input order preserved */
        }
    } else if(*cfg->dirs){ char*copy=xstrdup(cfg->dirs),*tok=strtok(copy,":");
        while(tok){ char d[PATH_MAX]; expand_tilde(tok,d,sizeof d); scan_dir(apps,d); tok=strtok(NULL,":"); }
        free(copy);
    } else {
        char dirs[64][PATH_MAX]; int nd=0; collect_default_dirs(dirs,&nd,64);
        for(int i=0;i<nd;i++) scan_dir(apps,dirs[i]);
    }
    if(!dmenu){ apply_config_order(apps,cfg); apply_recency(apps,cfg); }
}

/* dmenu's own flags. A script written for dmenu or bemenu calls the menu with
   -l 10 -p "" and the like, and used to get the app wheel: the flags were
   ignored, the pick was launched instead of printed, and the script got an
   empty answer. Any of these now means dmenu mode. Returns how many argv
   entries the flag takes, or 0 if it is not one of them. */
static int dmenu_flag(const char*a){
    static const char*with_arg[]={"-l","-p","-P","-fn","-nb","-nf","-sb","-sf",
                                  "-m","-w","-W","--prompt","--lines","--fn",0};
    static const char*bare[]={"-i","-b","-f","-n","--ignorecase",0};
    for(int k=0;with_arg[k];k++) if(!strcmp(a,with_arg[k])) return 2;
    for(int k=0;bare[k];k++) if(!strcmp(a,bare[k])) return 1;
    return 0;
}

int main(int argc,char**argv){
    Config cfg; config_defaults(&cfg);
    char cfgpath[PATH_MAX];
    
    xdg_path(cfgpath,sizeof cfgpath,"XDG_CONFIG_HOME",".config","config");

    int want_list=0, dmenu=0, do_replace=0;
    for(int i=1;i<argc;i++){
        if((!strcmp(argv[i],"-c")||!strcmp(argv[i],"--config"))&&i+1<argc)
            snprintf(cfgpath,sizeof cfgpath,"%s",argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){ usage(argv[0]); return 0; }
        else if(!strcmp(argv[i],"--replace")){ do_replace=1; }
        else if(!strcmp(argv[i],"-v")||!strcmp(argv[i],"--version")){
            printf("swas %s (build %s)\n",SWAS_VERSION,SWAS_BUILD); return 0; }
        else if(!strcmp(argv[i],"--dump-config")){ dump_config(); return 0; }
        else if(!strcmp(argv[i],"--list")) want_list=1;
        else if(!strcmp(argv[i],"--dmenu")||!strcmp(argv[i],"-d")) dmenu=1;
        else if(dmenu_flag(argv[i])){ dmenu=1; i+=dmenu_flag(argv[i])-1; }
    }
    { char tmp[PATH_MAX]; expand_tilde(cfgpath,tmp,sizeof tmp); snprintf(cfgpath,sizeof cfgpath,"%s",tmp); }
    if(do_replace) replace_running();   /* one wheel at a time */

    sw_shared_apply("swas",config_set_shared,&cfg);   /* shared first */
    config_load(&cfg,cfgpath);                            /* our own wins  */
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-c")||!strcmp(argv[i],"--config")){ i++; continue; }
        if(!strcmp(argv[i],"--list")||!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")||!strcmp(argv[i],"--dump-config")||!strcmp(argv[i],"--dmenu")||!strcmp(argv[i],"-d")||!strcmp(argv[i],"-v")||!strcmp(argv[i],"--version")||!strcmp(argv[i],"--replace")) continue;
        if(dmenu_flag(argv[i])){ i+=dmenu_flag(argv[i])-1; continue; }   /* and its value */
        if(!strcmp(argv[i],"--no-recent")||!strcmp(argv[i],"--all-apps")){ cfg.recent_first=0; continue; }
        char*a=argv[i]; while(*a=='-') a++;            /* accept --key=value too */
        char*eq=strchr(a,'='); if(eq){ *eq='\0'; config_set(&cfg,a,eq+1); }
    }
    if(cfg.ssaa<1)cfg.ssaa=1; if(cfg.ssaa>4)cfg.ssaa=4;
    if(cfg.slots<1)cfg.slots=1;
    { char tmp[PATH_MAX];
      expand_tilde(cfg.history,tmp,sizeof tmp); snprintf(cfg.history,sizeof cfg.history,"%s",tmp);
      if(*cfg.font){ expand_tilde(cfg.font,tmp,sizeof tmp); snprintf(cfg.font,sizeof cfg.font,"%s",tmp); } }

    AppList apps={0};

    if(want_list){                                 /* --list: no window needed */
        load_apps(&apps,&cfg,dmenu);
        for(int j=0;j<apps.n;j++)
            printf("%-26s | %-32s | icon=%s%s\n",apps.v[j].id,apps.v[j].name,
                   apps.v[j].icon?apps.v[j].icon:"-", apps.v[j].terminal?"  [term]":"");
        return 0;
    }

    /* A layer-shell surface on the overlay, the size of the output. Not a
       toplevel window: the compositor puts it above everything including a
       fullscreen client, it takes the keyboard for as long as it is up, and
       there is no float/center rule to write. */
    if(!sw_init()){ fprintf(stderr,"swas: %s\n",sw_error()); return 1; }

    SwWinOpts wo; memset(&wo,0,sizeof wo);
    wo.layer          = SW_LAYER_OVERLAY;
    wo.keyboard       = SW_KBD_EXCLUSIVE;
    wo.exclusive_zone = -1;
    wo.namespace_     = APP_ID;
    if(cfg.fullscreen){
        wo.anchor = SW_ANCHOR_TOP|SW_ANCHOR_BOTTOM|SW_ANCHOR_LEFT|SW_ANCHOR_RIGHT;
    } else {
        wo.w = cfg.width; wo.h = cfg.height;
    }
    SwApp wapp; memset(&wapp,0,sizeof wapp);      /* we drive our own frames */
    SwWin *win = sw_win_create(&wo,&wapp);
    if(!win){ fprintf(stderr,"swas: %s\n",sw_error()); return 1; }

    /* Get the surface configured NOW, before the (possibly slow, on a busy
       machine) .desktop scan and font load — so keystrokes typed during
       loading are queued for us by the compositor instead of being lost. */
    for(int i=0;i<40 && sw_win_width(win)==0;i++) sw_dispatch(20);

    load_apps(&apps,&cfg,dmenu);                    /* slow part; keystrokes queue meanwhile */

    if(apps.n==0){ fprintf(stderr,"swas: no %s\n", dmenu?"input on stdin":".desktop applications found");
                   sw_shutdown(); return 1; }

    UiFont font; memset(&font,0,sizeof font);
    char fontpath[PATH_MAX]={0};
    if(*cfg.font){                                   /* explicit: a path OR a family name */
        if(file_exists(cfg.font)) snprintf(fontpath,sizeof fontpath,"%s",cfg.font);
        else fc_match(cfg.font,fontpath,sizeof fontpath);
    }
    if(!*fontpath) fc_match("sans-serif",fontpath,sizeof fontpath);  /* the desktop default */
    if(!*fontpath) try_font_paths(fontpath,sizeof fontpath);         /* last-resort known paths */
    int atlas_px=(int)(cfg.font_px*cfg.ui_scale); if(atlas_px<10)atlas_px=10; if(atlas_px>200)atlas_px=200;
    if(*fontpath) font_load(&font,fontpath,atlas_px);
    if(!font.ok) fprintf(stderr,"swas: no usable TTF font found\n");

    int *filt=malloc(apps.n*sizeof*filt); int fn=0; char query[256]={0};
    int off=0, selslot=0;                 /* fixed highlight = off+selslot     */
    uint64_t anim0=0;                       /* time of last filter change (animation) */
    /* Per-slot crossfade bookkeeping: last_* is the layout drawn last frame;
       snap_* is frozen at the moment of a results change. A slot only fades if
       its app+position differ from the snapshot, so anything that stays put
       (e.g. the top icon while you refine a query) is not re-animated. */
    #define SNAPMAX 256
    int   snap_item[SNAPMAX], last_item[SNAPMAX]; float snap_ang[SNAPMAX], last_ang[SNAPMAX];
    int   snap_n=0, last_n=0, snap_sel=-1, last_sel=-1;
    #define REBUILD() do{ \
        memcpy(snap_item,last_item,sizeof(int)*last_n); memcpy(snap_ang,last_ang,sizeof(float)*last_n); \
        snap_n=last_n; snap_sel=last_sel; \
        fn=build_filter(&apps,query,filt,cfg.slots,cfg.recent_first,cfg.center_first,&off,&selslot); anim0=now_ms_(); }while(0)
    REBUILD();

    /* --- drag an app out of the wheel and onto a workspace ---
       A press only arms it. If the pointer stays put the release launches, as
       it always did; if it moves, the wheel shrinks out of the way and the
       workspaces appear along the top. Dropping keeps the wheel open. */
    int   dragging=0, press_down=0, press_item=-1, drop_ws=-1;
    float press_x=0, press_y=0;
    float wheel_scale=1.0f;               /* animated 1 -> drop_shrink */
    float corner_t=0.0f;                  /* 0 = centred, 1 = parked in a corner */
    uint64_t flash_at=0; int flash_ws=-1;   /* the tile that just took an app */
    Rect wsp_rect[24];

    int page_dir=0; float page_speed=0;   /* for the paging indicators         */
    float mx=cfg.width/2.0f,my=cfg.height/2.0f;
    uint64_t last_page=0, blink0=now_ms_();
    int running=1, had_focus=0, page_armed=0, prev_region=0, chose=0;
    int ov_started=0; float ov_lx=-1, ov_ly=-1;
    /* The compositor reports where the pointer already is as soon as the
       surface is up. That is not the user moving it, and it must not throw
       away the selection we opened with. */
    int mouse_live=0; float m0x=0, m0y=0; int mouse_seen=0;
    #define CHOOSE() do{ if(dmenu){ printf("%s\n",apps.v[filt[sel]].name); fflush(stdout); chose=1; } \
                         else launch(&apps.v[filt[sel]],&cfg); running=0; }while(0)
    float arc=cfg.arc_deg*(float)DEG; if(arc<60*DEG)arc=60*DEG; if(arc>330*DEG)arc=330*DEG;

    float ui=cfg.ui_scale;
    int redraw=1;                 /* something changed, so the frame is worth drawing */
    float want_scale=1.0f, want_corner=0.0f;

    while(running){
        if(!sw_dispatch(0)) break;              /* the compositor went away */
        int w=sw_win_width(win), h=sw_win_height(win);
        if(w<=0||h<=0){ sw_dispatch(16); continue; }

        {   /* ease the wheel down to its dragging size and place, and back.
               drop_size is a width in pixels, which is what you actually want
               to say — "about a hundred across" — rather than a fraction of a
               screen you have to work out. */
            float full = (float)(w<h?w:h);
            float want = 1.0f;
            if(dragging){
                want = cfg.drop_size>0 && cfg.radius>0.0f && full>0.0f
                     ? (float)cfg.drop_size/(2.0f*cfg.radius*full)
                     : cfg.drop_shrink;
                if(want<0.04f) want=0.04f;
                if(want>1.0f)  want=1.0f;
            }
            float step = cfg.drop_ms>0 ? 16.0f/(float)cfg.drop_ms : 1.0f;
            float cwant = dragging ? 1.0f : 0.0f;
            want_scale = want;
            want_corner = cwant;
            if(wheel_scale<want){ wheel_scale+=step; if(wheel_scale>want)wheel_scale=want; }
            else if(wheel_scale>want){ wheel_scale-=step; if(wheel_scale<want)wheel_scale=want; }

            if(corner_t<cwant){ corner_t+=step; if(corner_t>cwant)corner_t=cwant; }
            else if(corner_t>cwant){ corner_t-=step; if(corner_t<cwant)corner_t=cwant; }
        }

        float mind=(w<h?w:h)*wheel_scale;
        float R=mind*cfg.radius, ri=R*0.46f, rc=ri*0.95f, rl=(R+ri)/2.0f;
        float cx=w/2.0f, cy=h/2.0f + cfg.y_offset*mind;

        /* Out of the way. Small enough in the middle it stops covering what
           you are aiming at; a corner moves it further still. Either way it
           stays the place to drop an app you have changed your mind about. */
        int c_left  = strstr(cfg.drop_corner,"left")   != NULL;
        int c_right = strstr(cfg.drop_corner,"right")  != NULL;
        int c_top   = strstr(cfg.drop_corner,"top")    != NULL;
        int c_bot   = strstr(cfg.drop_corner,"bottom") != NULL;
        if((c_left||c_right||c_top||c_bot) && corner_t>0.0f){
            float pad=R*0.12f+14.0f*cfg.ui_scale;
            float tx = c_left ? R+pad : c_right ? (float)w-R-pad : cx;
            float ty = c_top  ? R+pad : c_bot   ? (float)h-R-pad : cy;
            float e=corner_t*corner_t*(3.0f-2.0f*corner_t);   /* smoothstep */
            cx += (tx-cx)*e;
            cy += (ty-cy)*e;
        }
        int over_wheel = dragging &&
            ((mx-cx)*(mx-cx)+(my-cy)*(my-cy)) < (R*1.12f)*(R*1.12f);

        /* the workspace strip across the top, laid out every frame so it
           follows a resize */
        float tile_h=0, tile_y=0;
        if(NWSP>0){
            float margin=w*0.035f, gap=w*0.007f;
            float avail=(float)w-2*margin-(float)(NWSP-1)*gap;
            float tw=avail/(float)NWSP;
            float maxw=230.0f*cfg.ui_scale;
            if(tw>maxw){ tw=maxw;
                margin=((float)w-(tw*(float)NWSP+(float)(NWSP-1)*gap))/2.0f; }
            tile_h=(float)h*0.135f; if(tile_h>190.0f*cfg.ui_scale) tile_h=190.0f*cfg.ui_scale;
            tile_y=(float)h*0.035f;
            for(int i=0;i<NWSP;i++)
                wsp_rect[i]=(Rect){ margin+(tw+gap)*(float)i, tile_y, tw, tile_h };
        }

        int searching = query[0]!=0;
        int vis = fn? (fn<cfg.slots?fn:cfg.slots) : 0;
        if(searching && vis>=4 && (vis&1)==0) vis--;   /* odd count -> a real top slot, balanced sides */
        int maxoff = fn>vis?fn-vis:0;
        if(off<0)off=0;
        if(off>maxoff)off=maxoff;
        if(vis>0){ if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
        int sel = fn? off+selslot : 0;
        if(sel>fn-1)sel=fn-1;

        float step = arc/(float)(vis>0?vis:1);   /* equal sectors fill the arc */
        float top=-(float)M_PI/2;
        float astart=top-arc/2;

        /* --- paging: hovering anywhere in the bottom zone pages; deeper toward
           straight-down = faster (slow -> fast). Gated by page_armed so it never
           fires from wherever the cursor happened to open on (startup safety). --- */
        float half_arc = arc/2 + 2.0f*(float)DEG;     /* apps fill the arc; page below it */
        page_dir=0; page_speed=0;
        if(page_armed && !dragging){
          float dx=mx-cx,dy=my-cy,dist=sqrtf(dx*dx+dy*dy);
          if(dist>rc){
              float pa=atan2f(dy,dx), dtop=norm_ang(pa-top);
              if(fabsf(dtop)>half_arc){               /* below the app arc = paging zone */
                  page_dir = dtop>0?1:-1;             /* right=forward, left=back */
                  float depth=fabsf(dtop)-half_arc, maxd=(float)M_PI-half_arc;
                  page_speed = maxd>0?clampf(depth/maxd,0,1):0;
                  if(fn>vis){
                      uint64_t now=now_ms_();
                      float t=page_speed*page_speed;   /* ease in: gentle start, fast finish */
                      int iv=(int)(cfg.page_ms*(1.0f-0.82f*t)); if(iv<24)iv=24;
                      if((int64_t)(now-last_page)>=iv){
                          off += page_dir;
                          if(off<0)off=0;
                          if(off>maxoff)off=maxoff;
                          last_page=now;
                      }
                  }
              }
          }
        }

        if(OV_OUT>=0) ov_poll();

        SwEvent ev;
        while(sw_poll(&ev)){
            redraw=1;
            if(ev.type==SW_EV_CLOSED) running=0;
            else if(ev.type==SW_EV_KB_ENTER) had_focus=1;
            else if(ev.type==SW_EV_KB_LEAVE){
                /* With the overview up, apps are being started behind us on
                   purpose; each one takes the focus for a moment and that is
                   not a reason to close. */
                if(cfg.close_on_focus_loss && had_focus && !OV_READY) running=0;
            }
            else if(ev.type==SW_EV_MOTION){
                mx=ev.x; my=ev.y;

                if(!mouse_live){
                    if(!mouse_seen){ mouse_seen=1; m0x=mx; m0y=my; }
                    else if(fabsf(mx-m0x)>2.0f||fabsf(my-m0y)>2.0f) mouse_live=1;
                    if(!mouse_live) continue;   /* the pointer has not moved yet */
                }

                if(press_down && !dragging && cfg.drop && press_item>=0){
                    float dx=mx-press_x, dy=my-press_y;
                    if(dx*dx+dy*dy > (float)(cfg.drop_px*cfg.drop_px)){
                        ov_bring_up(&cfg, 300);
                        if(OV_READY){
                            dragging=1;
                            ov_send("drag on");
                        }
                        else {
                            wsp_load(&cfg);      /* first drag pays for the list */
                            if(NWSP>0) dragging=1; else press_item=-1;
                        }
                    }
                }
                if(dragging){
                    /* Over the wheel means "never mind": the overview is told
                       there is no target, so a workspace underneath the wheel
                       is not picked up by accident. */
                    if(OV_READY){
                        if(mx!=ov_lx||my!=ov_ly){         /* ask the overview */
                            ov_lx=mx; ov_ly=my;
                            if(over_wheel){ ov_send("hover -1 -1"); OV_TARGET[0]='\0'; }
                            else ov_send("hover %.4f %.4f",mx/(float)w,my/(float)h);
                        }
                    } else {
                        drop_ws=-1;
                        if(!over_wheel)
                            for(int i=0;i<NWSP;i++){
                                Rect t=wsp_rect[i];
                                if(mx>=t.x&&mx<t.x+t.w&&my>=t.y&&my<t.y+t.h){ drop_ws=i; break; }
                            }
                    }
                    continue;                    /* no slot hover while dragging */
                }

                float dx=mx-cx,dy=my-cy,dist=sqrtf(dx*dx+dy*dy);
                int region=1; float adtop=0;   /* 1=apps/center, 2=bottom off-icon, 3=on icon */
                if(dist>rc){
                    float pa=atan2f(dy,dx), dtop=norm_ang(pa-top);
                    adtop=fabsf(dtop);
                    if(adtop<=half_arc){                    /* within the app arc */
                        if(vis>0){
                            int slot = (int)floorf((dtop+arc/2)/(step>0?step:1));
                            if(slot<0)slot=0; if(slot>vis-1)slot=vis-1;
                            selslot=slot;
                        }
                    }
                    else if(on_icon(mx,my,cx,cy,rl,R*0.10f)) region=3;   /* on the icon */
                    else region=2;                                       /* bottom, off icon */
                }
                /* Startup safety (see the 3-state rules):
                     - reaching the apps/center always arms (state 1)
                     - approaching the icon from the bottom arms (state 2 -> icon)
                     - leaving the icon DOWN/SIDEWAYS into the bottom arms; leaving
                       it UP toward the apps does NOT, so you can pick an app
                       without scrolling. Once armed, the whole bottom scrolls. */
                float icon_dtop=fabsf(norm_ang(ICON_AL-top));   /* ~150 deg */
                if(region==1) page_armed=1;
                else if(region==3 && prev_region==2) page_armed=1;
                else if(region==2 && prev_region==3 && adtop>=icon_dtop) page_armed=1;
                prev_region=region;
            }
            else if(ev.type==SW_EV_AXIS){
                /* Wayland scrolls in surface units, positive downward. */
                int dir=ev.ay<0?-1:(ev.ay>0?1:0);
                if(!dir&&ev.ax)dir=ev.ax>0?1:-1;
                if(dir){
                    if(maxoff>0){ off+=dir; if(off<0)off=0; if(off>maxoff)off=maxoff; }
                    else if(vis>0){ selslot+=dir; if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
                }
            }
            else if(ev.type==SW_EV_BUTTON && ev.pressed){
                if(ev.button==SW_BTN_LEFT&&fn){
                    press_down=1; press_x=ev.x; press_y=ev.y;
                    /* the bottom zone is for paging; nothing is dragged there */
                    press_item=(prev_region==2||prev_region==3)?-1:filt[sel];
                }
                else if(ev.button==SW_BTN_RIGHT){
                    if(dragging){ dragging=press_down=0; drop_ws=-1; press_item=-1;
                                  if(OV_READY){ ov_send("drag off"); OV_TARGET[0]='\0'; } }
                    else running=0;
                }
            }
            else if(ev.type==SW_EV_BUTTON && !ev.pressed){
                if(ev.button==SW_BTN_LEFT){
                    if(dragging){
                        if(OV_READY){
                            if(over_wheel) OV_TARGET[0]='\0';   /* changed my mind */
                            if(OV_TARGET[0] && press_item>=0){
                                pid_t p=launch(&apps.v[press_item],&cfg);
                                /* Straight down the pipe we already have, so
                                   sway knows where the window belongs before
                                   the app has finished starting: it never
                                   lands here first and nothing on this
                                   workspace gets rearranged. */
                                if(p>0) ov_send("assign %d %s",(int)p,OV_TARGET);
                                adopt_to(&cfg,p,OV_TARGET,OV_BESIDE,OV_EDGE);
                                /* the new window takes the focus as it
                                   opens, but it is an ordinary window and
                                   the wheel is a layer surface above it, so
                                   the wheel stays in front by itself */
                                if(OV_ONCE) running=0;     /* one go */
                                else if(OV_HERE && cfg.drop_close_here) running=0;
                            }
                            ov_send("drag off");
                            OV_TARGET[0]='\0'; OV_HERE=0;
                            OV_BESIDE[0]='\0'; OV_EDGE[0]='\0'; ov_lx=ov_ly=-1;
                        } else if(!over_wheel && drop_ws>=0 && press_item>=0){
                            pid_t p=launch(&apps.v[press_item],&cfg);
                            wsp_adopt(&cfg,p,&WSP[drop_ws]);
                            WSP[drop_ws].exists=1;      /* it will be there now */
                            flash_ws=drop_ws; flash_at=now_ms_();
                            if(WSP[drop_ws].focused && cfg.drop_close_here) running=0;
                        }
                        /* dropped on the wheel, or nowhere: nothing happens */
                        dragging=0; drop_ws=-1;
                    } else if(press_down && fn){
                        CHOOSE();
                    }
                    press_down=0; press_item=-1;
                }
            }
            else if(ev.type==SW_EV_KEY && ev.pressed){
                uint32_t k=ev.keysym;
                if(k==SW_KEY_ESCAPE){
                    if(dragging){ dragging=press_down=0; drop_ws=-1; press_item=-1;
                                  if(OV_READY){ ov_send("drag off"); OV_TARGET[0]='\0'; } }
                    else if(query[0]){ query[0]='\0'; REBUILD(); }
                    else running=0;
                }
                else if(k==SW_KEY_RETURN||k==SW_KEY_KP_ENTER){ if(fn){ CHOOSE(); } }
                else if(k==SW_KEY_BACKSPACE){ int L=strlen(query);
                    if(L>0){ L--; while(L>0&&((unsigned char)query[L]&0xC0)==0x80)L--; query[L]='\0'; REBUILD(); } }
                else if(k==SW_KEY_RIGHT || (k==SW_KEY_TAB && !(ev.mods&SW_MOD_SHIFT))){
                    if(vis>0){ if(selslot<vis-1)selslot++; else if(off<maxoff)off++; } }
                else if(k==SW_KEY_LEFT || (k==SW_KEY_TAB && (ev.mods&SW_MOD_SHIFT))){
                    if(vis>0){ if(selslot>0)selslot--; else if(off>0)off--; } }
                else if(k==SW_KEY_DOWN){ if(maxoff>0){ off+=vis; if(off>maxoff)off=maxoff; } }
                else if(k==SW_KEY_UP){ if(maxoff>0){ off-=vis; if(off<0)off=0; } }
                /* Anything that types a character types into the query. The
                   keymap says what that is, so a German keyboard puts ä in
                   the box when ä is pressed. */
                else if(ev.text[0] && (unsigned char)ev.text[0]>=0x20){
                    strncat(query,ev.text,sizeof query-strlen(query)-1);
                    REBUILD();
                }
            }
        }
        if(!running) break;

        /* Input above may have changed fn/off/selslot (typing, backspace, paging),
           so recompute the whole layout before rendering — otherwise this frame
           would draw with the previous match count and the highlight flickers. */
        searching = query[0]!=0;
        vis = fn? (fn<cfg.slots?fn:cfg.slots) : 0;
        if(searching && vis>=4 && (vis&1)==0) vis--;
        maxoff = fn>vis?fn-vis:0;
        if(off<0)off=0;
        if(off>maxoff)off=maxoff;
        if(vis>0){ if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
        sel = fn? off+selslot : 0;
        if(sel>fn-1)sel=fn-1;
        step = arc/(float)(vis>0?vis:1);

        /* ------------------------------ render ---------------------------- */
        /* Every primitive is antialiased as it is drawn — the sectors come
           out of a distance field, the triangles out of half-plane distances,
           the glyphs out of stb_truetype — so the old supersampled render
           target is gone and `ssaa` is ignored.
           A frame is only drawn when something has changed or something is
           still moving: a wheel sitting still costs nothing. */
        {
            int moving = (wheel_scale!=want_scale) || (corner_t!=want_corner)
                       || page_dir!=0 || ERR_TEXT[0]!=0 || WATCH_PID!=0;
            if(cfg.animate && cfg.anim_ms>0 &&
               (int64_t)(now_ms_()-anim0) < (int64_t)cfg.anim_ms) moving=1;
            if(!redraw && !moving){ sw_dispatch(16); continue; }
        }

        Canvas *ren = sw_win_begin(win);
        if(!ren){ sw_dispatch(8); continue; }   /* both buffers still on screen */

        /* With swov behind us it draws its own scrim; a second one over the
           top only makes the thing you are aiming at harder to see. */
        if(!OV_READY) sw_canvas_bg(ren,cfg.bg);

        float slotw=step;
        /* everything inside the wheel is measured against this, so shrinking
           the wheel shrinks its contents by the same amount and it reads as
           the same picture, just smaller. The workspace tiles and the app on
           the pointer keep the real ui scale. */
        float uiw=ui*wheel_scale;
        float label_px=cfg.label_px*uiw;
        /* app-data crossfade: on a results change, the names/icons/highlight text
           fade back in while the ring sectors (background) stay put — smooths fast
           typing without delaying anything. */
        float af = (cfg.animate && cfg.anim_ms>0)
                   ? clampf(0.30f + 0.70f*(float)(now_ms_()-anim0)/(float)cfg.anim_ms, 0.0f, 1.0f) : 1.0f;

        /* Small, over a busy overview, the wheel had nothing to sit on and
           read as a handful of loose marks. A disc behind it gives it an edge
           again — only while it is small, fading in with the shrink. */
        if(corner_t>0.0f && cfg.drop_bg.a){
            Col d=cfg.drop_bg;
            d.a=(uint8_t)(d.a*clampf(corner_t,0.0f,1.0f));
            fill_circle(ren,cx,cy,R*1.10f,d);
        }

        for(int i=0;i<vis;i++){
            int item=filt[off+i];
            float a = astart+(i+0.5f)*step;
            float a0=a-slotw/2+0.010f, a1=a+slotw/2-0.010f;
            int hot=(off+i==sel);
            Col scol=hot?cfg.hl:(i&1?cfg.ring2:cfg.ring);
            fill_sector(ren,cx,cy,ri,R,a0,a1,scol);            /* sector stays solid */

            /* stationary if this exact app sat at (nearly) this angle before the change */
            int stationary=0;
            for(int k=0;k<snap_n;k++) if(snap_item[k]==item && fabsf(snap_ang[k]-a)<0.02f){ stationary=1; break; }
            float sfa = stationary?1.0f:af; uint8_t sfab=(uint8_t)(255*sfa);
            if(i<SNAPMAX){ last_item[i]=item; last_ang[i]=a; }

            float lx=cx+rl*cosf(a), ly=cy+rl*sinf(a);
            Col tc=hot?cfg.hltext:cfg.text; tc.a=(uint8_t)(tc.a*sfa);   /* text fades (unless it held still) */
            float chord=2*rl*sinf(slotw/2)*0.84f;
            char lbl[160]; fit_label(&font,label_px,apps.v[item].name,chord,lbl,sizeof lbl);

            Image ic=app_icon(&apps.v[item],&cfg);
            if(ic.rgba){
                float isz=cfg.icon_px*uiw; if(isz>chord)isz=chord;
                float gap=7.0f*uiw;                       /* breathing room */
                float blockH=isz+gap+label_px;
                float topy=ly-blockH/2;
                /* the icon fades with the rest, unless it held still */
                sw_blit(ren,ic,lx-isz/2,topy,isz,isz,sfab/255.0f);
                text_centered(ren,&font,lx, topy+isz+gap+label_px/2, label_px, tc, lbl);
            } else {
                text_centered(ren,&font,lx,ly,label_px,tc,lbl);
            }
        }
        last_n = vis<SNAPMAX?vis:SNAPMAX;

        /* paging indicators (chevrons) — brighten & multiply with speed */
        if(fn>vis){
            float aL=ICON_AL, aR=ICON_AR;   /* bottom-left / -right paging icons */
            float lxp=cx+rl*cosf(aL), lyp=cy+rl*sinf(aL);
            float rxp=cx+rl*cosf(aR), ryp=cy+rl*sinf(aR);
            /* left */
            { int on=(page_dir<0); float sp=on?page_speed:0;
              Col c=on?cfg.accent:cfg.dim; c.a=on?255:210;
              int cnt=1+(on?(int)lroundf(sp*2):0);
              float s=(19.0f+ (on?13.0f*sp:0))*uiw;
              draw_chevrons(ren,lxp,lyp,-1,s,cnt,c); }
            /* right */
            { int on=(page_dir>0); float sp=on?page_speed:0;
              Col c=on?cfg.accent:cfg.dim; c.a=on?255:210;
              int cnt=1+(on?(int)lroundf(sp*2):0);
              float s=(19.0f+ (on?13.0f*sp:0))*uiw;
              draw_chevrons(ren,rxp,ryp,+1,s,cnt,c); }
        }

        /* center hub */
        fill_circle(ren,cx,cy,rc,cfg.center);
        uint64_t now=now_ms_(); int caret=((now-blink0)/500)%2==0;
        char shown[300];
        if(query[0]) snprintf(shown,sizeof shown,"%s%s",query,caret?"|":" ");
        else         snprintf(shown,sizeof shown,"type to search");
        Col qc=query[0]?cfg.accent:cfg.dim;
        text_centered(ren,&font,cx,cy-rc*0.42f,cfg.search_px*uiw,qc,shown);

        if(fn){
            int sel_item=filt[sel]; last_sel=sel_item;
            float caf = (sel_item==snap_sel)?1.0f:af;      /* same app in the hub -> no fade */
            char nm[200]; fit_label(&font,cfg.title_px*uiw,apps.v[sel_item].name,rc*1.7f,nm,sizeof nm);
            Col nmc=cfg.text; nmc.a=(uint8_t)(nmc.a*caf);
            text_centered(ren,&font,cx,cy,cfg.title_px*uiw,nmc,nm);
            char cnt[64]; snprintf(cnt,sizeof cnt,"%d / %d",sel+1,fn);
            Col cc=cfg.dim; cc.a=(uint8_t)(cc.a*caf);
            text_centered(ren,&font,cx,cy+rc*0.40f,cfg.count_px*uiw,cc,cnt);
        } else {
            last_sel=-1;
            text_centered(ren,&font,cx,cy,cfg.title_px*uiw,cfg.dim,"no matches");
        }

        /* --- the workspaces, and the app hanging off the pointer --- */
        if(!OV_READY && (dragging || (flash_ws>=0 && now_ms_()-flash_at<420))){
            float rad=10.0f*ui;
            for(int i=0;i<NWSP;i++){
                Rect t=wsp_rect[i];
                int hot=(i==drop_ws), lit=(i==flash_ws && now_ms_()-flash_at<420);
                Col fill=hot||lit?cfg.hl:(WSP[i].exists?cfg.ring:cfg.bg);
                if(!WSP[i].exists && !hot && !lit) fill.a=0;

                if(fill.a) fill_round_rect(ren,t.x,t.y,t.w,t.h,rad,fill);
                if(!WSP[i].exists && !hot && !lit)
                    stroke_rect(ren,t.x,t.y,t.w,t.h,fmaxf(1.0f,1.5f*ui),cfg.dim);
                else if(WSP[i].focused && !hot && !lit)
                    stroke_rect(ren,t.x,t.y,t.w,t.h,fmaxf(1.0f,2.0f*ui),cfg.accent);

                Col tc = hot||lit ? cfg.hltext : (WSP[i].exists?cfg.text:cfg.dim);
                char num[16]; snprintf(num,sizeof num,"%d",WSP[i].num);
                const char*lbl=wsp_label(&WSP[i]);
                float ny = *lbl ? t.y+t.h*0.40f : t.y+t.h*0.5f;
                text_centered(ren,&font,t.x+t.w/2,ny,cfg.title_px*ui*1.15f,tc,num);
                if(*lbl){
                    char cut[80]; fit_label(&font,cfg.count_px*ui,lbl,t.w*0.86f,cut,sizeof cut);
                    text_centered(ren,&font,t.x+t.w/2,t.y+t.h*0.72f,cfg.count_px*ui,tc,cut);
                }
            }
        }
        if(over_wheel){                      /* the wheel says: let go here to stop */
            Col o=cfg.dim; o.a=200;
            ring(ren,cx,cy,R*1.12f,fmaxf(2.0f,2.0f*ui),o);
        }
        if(dragging && press_item>=0){
            App*a=&apps.v[press_item];
            float isz=cfg.icon_px*ui*1.15f;
            Image ic=app_icon(a,&cfg);
            if(ic.rgba) sw_blit(ren,ic,mx-isz/2,my-isz/2,isz,isz,235/255.0f);
            char cut[160]; fit_label(&font,cfg.label_px*ui,a->name,300.0f*ui,cut,sizeof cut);
            text_centered(ren,&font,mx,my+isz*0.72f,cfg.label_px*ui,cfg.text,cut);
        }

        /* Whatever went wrong, said plainly under the wheel. */
        watch_tick();
        if(ERR_TEXT[0] && now_secs_() < ERR_UNTIL){
            float a=(float)(ERR_UNTIL-now_secs_());
            if(a>1.0f) a=1.0f;
            Col ec=cfg.hl; ec.a=(uint8_t)(255*a);
            Col bgc=cfg.center; bgc.a=(uint8_t)(bgc.a*a);
            char cut[200];
            fit_label(&font,cfg.label_px*ui,ERR_TEXT,(float)w*0.7f,cut,sizeof cut);
            float tw=text_width(&font,cfg.label_px*ui,cut);
            float pad=14.0f*ui, bh=cfg.label_px*ui+pad;
            sw_fill_rect(ren,(float)w*0.5f-tw*0.5f-pad, cy+R+40.0f*ui, tw+2*pad, bh, bgc);
            float boxy=cy+R+40.0f*ui;
            text_centered(ren,&font,(float)w*0.5f,boxy+bh*0.5f+cfg.label_px*ui*0.35f,
                          cfg.label_px*ui,ec,cut);
        } else if(ERR_TEXT[0] && now_secs_() >= ERR_UNTIL) {
            ERR_TEXT[0]=0;
        }

        sw_win_end(win);
        redraw=0;

        if(!ov_started && cfg.drop){
            ov_started=1;             /* only now: the wheel is already up */
            /* An overview that started us is already there. One of our own
               waits until an app is being dragged — see ov_bring_up. */
            ov_adopt();
        }
        sw_dispatch(16);            /* ~60 fps, and the compositor's events */
    }
    ov_stop();

    sw_fonts_free();
    sw_shutdown();
    return (dmenu && !chose) ? 1 : 0;   /* dmenu convention: non-zero on cancel */
}
