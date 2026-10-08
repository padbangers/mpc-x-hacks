/* shim_remap10.so - config-driven MPC surface-button remapper with single/double/long press, touch taps and a pad
 * bank lock.
 *
 * v10 (ideas from mmiroshnikov/akai_standalone_remap, MIT): long-press rules, touchscreen taps, remap.conf re-read
 * when it changes (no app restart), and one injection queue for anything that fires without a surface event.
 *
 *   long <note> <mode> <arg>   held past longms: runs this; a short press then runs on release (its plain rule, or
 *                              the button itself when there is none)
 *   <note> touch X,Y           taps the touchscreen at screen pixel X,Y (1280x800, landscape as seen)
 *   <note> macro STEPS         steps separated by '/': bN button tap, pN pad tap, mN Menu + pad N, tX,Y touch tap,
 *                              wMS wait (N: decimal or 0x hex), e.g. macro b0x34/w300/t110,770/w500/t400,300
 *   longms 400 / touchms 60    long-press threshold / how long a touch tap is held
 * Earlier header (v9):
 *
 * Virtual hardware-type seq input "Padbangers"; app auto-subscribes + control-enables it.
 * Mapped surface buttons are suppressed; the shim emits a CC out of Padbangers that the
 * app's global mapping (.xmm) turns into an action/page.
 *
 * remap.conf line:  <noteHex> <mode> <cc> [<mode2> <cc2>]
 *   3 fields  = single action only (fires immediately)
 *   5 fields  = single action + double-click action (single is delayed by the click window)
 *   mode = rel+ (send val 1) | rel- (send val 127) | btn (send 127 then 0, momentary)
 *   cc must be bound to a Target_control in Track Remap.xmm.
 *   e.g.:  36 rel+ 74                 (+ -> track +1)
 *          2a btn 101 rel+ 61         (Next Seq: 1x -> CC101(Song), 2x -> CC61 +1(next seq))
 * Bank lock (remap.conf "banklock 1", the default; "banklock 0" turns it off): the app switches the pad bank by itself
 * when Grid view or Pad Perform opens (to C). The shim watches the bank LEDs the app writes (b0 23..26 vv, vv != 0 =
 * lit) and, when the app lights another bank than the one the user last pressed (A at start), injects a press of the
 * user's bank in front of the next surface event, so the next pad hit already plays in the user's bank. The app reads
 * the surface only when the controller sends something, so the correction lands with the next button/pad press.
 * Shift+bank (E-H) suspends the lock until a plain A-D press.
 * Env: REMAP_CH(0) REMAP_PORTNAME(Padbangers) REMAP_CONF(/data/hacks/remap.conf) REMAP_DCLICK_MS(350)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/input.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_seq snd_seq_t;
typedef unsigned char snd_seq_event_type_t;
typedef struct snd_seq_addr { unsigned char client, port; } snd_seq_addr_t;
typedef struct snd_seq_real_time { unsigned int tv_sec, tv_nsec; } snd_seq_real_time_t;
typedef union snd_seq_timestamp { unsigned int tick; snd_seq_real_time_t time; } snd_seq_timestamp_t;
typedef struct snd_seq_ev_note { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } snd_seq_ev_note_t;
typedef struct snd_seq_ev_ctrl { unsigned char channel; unsigned char unused[3]; unsigned int param; signed int value; } snd_seq_ev_ctrl_t;
typedef struct snd_seq_ev_raw8 { unsigned char d[12]; } snd_seq_ev_raw8_t;
typedef struct snd_seq_event {
    snd_seq_event_type_t type; unsigned char flags, tag, queue;
    snd_seq_timestamp_t time; snd_seq_addr_t source; snd_seq_addr_t dest;
    union { snd_seq_ev_note_t note; snd_seq_ev_ctrl_t control; snd_seq_ev_raw8_t raw8; } data;
} snd_seq_event_t;

#define SND_SEQ_OPEN_DUPLEX 3
#define SND_SEQ_PORT_CAP_READ      (1<<0)
#define SND_SEQ_PORT_CAP_WRITE     (1<<1)
#define SND_SEQ_PORT_CAP_SUBS_READ (1<<4)
#define SND_SEQ_PORT_CAP_SUBS_WRITE (1<<5)
#define SND_SEQ_PORT_TYPE_MIDI_GENERIC (1<<1)
#define SND_SEQ_PORT_TYPE_HARDWARE     (1<<16)
#define SND_SEQ_EVENT_CONTROLLER 10
#define SND_SEQ_QUEUE_DIRECT 253
#define SND_SEQ_ADDRESS_SUBSCRIBERS 254
#define SND_SEQ_ADDRESS_UNKNOWN 253

enum { M_RELP=0, M_RELM=1, M_BTN=2, M_PROBE=3, M_NOTE=4, M_NOTE2=5, M_NPROBE=6, M_MENUPAD=7, M_CPROBE=8, M_TOUCH=9, M_MACRO=10, M_NONE=-1 };
#define MENU_CODE 123
static int probe_cur=-1, probe_start=90, probe_end=118;
static int nprobe_cur=0, nprobe_start=0, nprobe_end=127;
typedef struct {
    int note;
    int is_shift;                 /* 1 = only active while Shift is held */
    int s_mode, s_cc;
    int has_d, d_mode, d_cc;
    int armed; double t_ms;
    int s_x, s_y, d_x, d_y;       /* touch targets (screen pixels) */
    int has_long, l_mode, l_cc, l_x, l_y;
    int held, long_fired; double down_t;
    char s_str[96], d_str[96], l_str[96];   /* macro steps */
} mapping_t;
static int long_ms=400, touch_ms=60;
static time_t conf_mtime=0;
static int shift_note=0x31;        /* Shift button surface note (config: "shift 31") */
static volatile int shift_held=0;
static int shift_suppress=1;       /* 1 = hide Shift from the app (config: "shiftsuppress 0/1") */
static mapping_t maps[128]; static int nmaps=0;

/* bank lock state */
#define BANK_A 0x23
#define BANK_D 0x26
static int banklock = 1;
static volatile int user_bank = BANK_A;      /* the bank the user last chose (A-D) */
static volatile int bank_suspended = 0;      /* 1 after Shift+bank (E-H): don't interfere */
static volatile int pending_bank = -1;       /* a press to inject before the next surface event */
static volatile double last_bank_press = 0, last_bank_inject = 0;
/* v9: the correction goes in at once. The app's MIDI thread waits in poll() on the surface's fd; that poll also
   watches wake_pipe, which bank_led() writes to, so the thread wakes up and reads; the read hook then hands it the
   injected press even when the surface has nothing new. */
static int surface_fd = -1;
static int wake_pipe[2] = { -1, -1 };
static int poll_seen = 0;
static ssize_t (*orig_rmwrite)(snd_rawmidi_t*, const void*, size_t) = NULL;
static int (*orig_rmopen)(snd_rawmidi_t**, snd_rawmidi_t**, const char*, int) = NULL;
static ssize_t (*orig_rmread)(snd_rawmidi_t*, void*, size_t) = NULL;
static int (*orig_poll)(struct pollfd*, nfds_t, int) = NULL;
static int (*orig_seqopen)(snd_seq_t**, const char*, int, int) = NULL;
static int (*orig_clientid)(snd_seq_t*) = NULL;
static int (*orig_setname)(snd_seq_t*, const char*) = NULL;
static int (*orig_createport)(snd_seq_t*, const char*, unsigned int, unsigned int) = NULL;
static int (*orig_output)(snd_seq_t*, snd_seq_event_t*) = NULL;
static int (*orig_drain)(snd_seq_t*) = NULL;

static int active = 0;   /* 1 only in the MPC app process */
static void *surface_in = NULL;
static snd_seq_t *v_seq = NULL;  static int v_client=-1, v_port=-1;
static int inj_ch=0, dclick_ms=350;
static const char *portname="Padbangers", *confpath="/data/hacks/remap.conf";
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

static FILE *lg=NULL;
static void L(const char*fmt,...){ if(!lg){lg=fopen("/data/hacks/remap.log","a"); if(!lg)return; setvbuf(lg,NULL,_IONBF,0);} va_list ap; va_start(ap,fmt); vfprintf(lg,fmt,ap); va_end(ap);}

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000.0 + ts.tv_nsec/1e6; }
static int parse_mode(const char*s){ return !strcmp(s,"macro")?M_MACRO : !strcmp(s,"touch")?M_TOUCH : !strcmp(s,"rel+")?M_RELP : !strcmp(s,"rel-")?M_RELM : !strcmp(s,"btn")?M_BTN : !strcmp(s,"probe")?M_PROBE : !strcmp(s,"note")?M_NOTE : !strcmp(s,"note2")?M_NOTE2 : !strcmp(s,"nprobe")?M_NPROBE : !strcmp(s,"menupad")?M_MENUPAD : !strcmp(s,"cprobe")?M_CPROBE : M_NONE; }

/* an action argument: a decimal number (CC, note or pad, as in v9), or X,Y for touch */
static void parse_arg(int mode, const char*a, int*v, int*x, int*y, char*str){
    *v=0; *x=0; *y=0; str[0]=0;
    if(mode==M_MACRO){ snprintf(str,96,"%s",a); return; }
    if(mode==M_TOUCH){ sscanf(a,"%d,%d",x,y); return; }
    *v=atoi(a);
}

static mapping_t* map_for(int note, int is_shift){
    for(int i=0;i<nmaps;i++) if(maps[i].note==note && maps[i].is_shift==is_shift) return &maps[i];
    if(nmaps>=128) return NULL;
    mapping_t*mp=&maps[nmaps++]; memset(mp,0,sizeof *mp);
    mp->note=note; mp->is_shift=is_shift; mp->s_mode=M_NONE;
    return mp;
}

static void load_conf(void){
    nmaps=0; FILE*f=fopen(confpath,"r"); if(!f){ L("conf %s not found\n",confpath); return; }
    struct stat st; if(fstat(fileno(f),&st)==0) conf_mtime=st.st_mtime;
    char line[160];
    while(fgets(line,sizeof line,f)){
        char*p=line; while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p==0) continue;
        char t[6][96]; memset(t,0,sizeof t);
        int k=sscanf(p,"%95s %95s %95s %95s %95s %95s",t[0],t[1],t[2],t[3],t[4],t[5]);
        if(k>=2 && !strcmp(t[0],"shift")){ shift_note=(int)strtol(t[1],0,16); L("conf: shift note = 0x%02x\n",shift_note); continue; }
        if(k>=2 && !strcmp(t[0],"shiftsuppress")){ shift_suppress=atoi(t[1]); continue; }
        if(k>=2 && !strcmp(t[0],"banklock")){ banklock=atoi(t[1]); continue; }
        if(k>=2 && !strcmp(t[0],"longms")){ long_ms=atoi(t[1]); continue; }
        if(k>=2 && !strcmp(t[0],"touchms")){ touch_ms=atoi(t[1]); continue; }
        int lng = !strcmp(t[0],"long");
        char (*r)[96] = lng ? t+1 : t;          /* the rule's tokens: note mode arg [mode2 arg2] */
        int rk = lng ? k-1 : k;
        if(rk<3) continue;
        int is_shift=(r[0][0]=='s'||r[0][0]=='S');
        int note=(int)strtol(is_shift?r[0]+1:r[0],0,16);
        int m1=parse_mode(r[1]); if(m1==M_NONE){ L("conf bad mode %s\n",r[1]); continue; }
        mapping_t*mp=map_for(note,is_shift); if(!mp) continue;
        if(lng){
            mp->has_long=1; mp->l_mode=m1; parse_arg(m1,r[2],&mp->l_cc,&mp->l_x,&mp->l_y,mp->l_str);
            L("conf: %snote 0x%02x long(%s %s)\n", is_shift?"SHIFT+":"", note, r[1], r[2]);
            continue;
        }
        mp->s_mode=m1; parse_arg(m1,r[2],&mp->s_cc,&mp->s_x,&mp->s_y,mp->s_str); mp->armed=0; mp->has_d=0;
        if(m1==M_PROBE){ probe_start=mp->s_cc; probe_cur=mp->s_cc; }
        if(m1==M_NPROBE){ nprobe_start=mp->s_cc; nprobe_cur=mp->s_cc; }
        if(m1==M_CPROBE){ probe_start=mp->s_cc; probe_cur=mp->s_cc; probe_end=89; }
        if(rk>=5){ int m2=parse_mode(r[3]); if(m2!=M_NONE){ mp->has_d=1; mp->d_mode=m2; parse_arg(m2,r[4],&mp->d_cc,&mp->d_x,&mp->d_y,mp->d_str); } }
        L("conf: %snote 0x%02x single(%s %s)%s\n", is_shift?"SHIFT+":"", note, r[1], r[2], mp->has_d?" +double":"");
    }
    fclose(f); L("conf: %d mapping(s), longms %d, touchms %d\n", nmaps, long_ms, touch_ms);
}

/* remap.conf changed? Re-read it (checked at most twice a second, on surface presses). */
static void maybe_reload(void){
    static double last=0; double now=now_ms();
    if(now-last<500) return;
    last=now;
    struct stat st; if(stat(confpath,&st)!=0 || st.st_mtime==conf_mtime) return;
    pthread_mutex_lock(&lk); load_conf(); pthread_mutex_unlock(&lk);
    L("[pid %d] remap.conf changed: re-read\n",getpid());
}

static void make_vport(void){
    if(v_seq || !orig_seqopen) return;
    if(orig_seqopen(&v_seq,"default",SND_SEQ_OPEN_DUPLEX,0)!=0){ v_seq=NULL; L("vport open failed\n"); return; }
    if(orig_setname) orig_setname(v_seq, portname);
    if(orig_clientid) v_client=orig_clientid(v_seq);
    if(orig_createport) v_port=orig_createport(v_seq, portname,
        SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_READ|SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_HARDWARE);
    L("[pid %d] vport '%s' client=%d port=%d\n", getpid(), portname, v_client, v_port);
}

static void send_cc(int cc,int value){
    if(!v_seq || !orig_output) return;
    snd_seq_event_t ev; memset(&ev,0,sizeof ev);
    ev.type=SND_SEQ_EVENT_CONTROLLER; ev.flags=0; ev.queue=SND_SEQ_QUEUE_DIRECT;
    ev.source.port=(unsigned char)(v_port<0?0:v_port);
    ev.dest.client=SND_SEQ_ADDRESS_SUBSCRIBERS; ev.dest.port=SND_SEQ_ADDRESS_UNKNOWN;
    ev.data.control.channel=(unsigned char)inj_ch; ev.data.control.param=cc; ev.data.control.value=value;
    orig_output(v_seq,&ev); if(orig_drain) orig_drain(v_seq);
}
static void fire(int mode,int cc){
    if(mode==M_RELP) send_cc(cc,1);
    else if(mode==M_RELM) send_cc(cc,127);
    else if(mode==M_BTN){ send_cc(cc,127); usleep(15000); send_cc(cc,0); }
}

/* ---- injection queue: surface bytes for actions that fire without a surface event (long press, timers). The read
   hook puts them in front of the next read; poll() reports the surface readable while any are queued. ---- */
static unsigned char injq[512]; static int injq_n=0;
static pthread_mutex_t injq_lk = PTHREAD_MUTEX_INITIALIZER;
static int has_pending(void){ return injq_n>0 || pending_bank>=0; }
static void enqueue(const unsigned char*b, int n){
    pthread_mutex_lock(&injq_lk);
    if(injq_n+n<=(int)sizeof injq){ memcpy(injq+injq_n,b,n); injq_n+=n; }
    pthread_mutex_unlock(&injq_lk);
    if(wake_pipe[1]>=0){ ssize_t r=write(wake_pipe[1],"x",1); (void)r; }
}
static int drain_queue(unsigned char*out, int cap){
    pthread_mutex_lock(&injq_lk);
    int n = injq_n<cap ? injq_n : cap;
    memcpy(out,injq,n); memmove(injq,injq+n,injq_n-n); injq_n-=n;
    pthread_mutex_unlock(&injq_lk);
    return n;
}

/* ---- touchscreen taps: written to the touch device as the events a finger makes (multitouch protocol B, one slot).
   The device is found by its capabilities (its event number changes between boots). Raw axes are portrait
   (0..800 x 0..1280); the screen as seen is landscape: raw_x = xmax - y*xmax/800, raw_y = x*ymax/1280. ---- */
static int touch_fd=-1, touch_xmax=800, touch_ymax=1280, touch_id=1000;
static void touch_open(void){
    if(touch_fd>=0) return;
    char path[32];
    for(int i=0;i<10;i++){
        snprintf(path,sizeof path,"/dev/input/event%d",i);
        int fd=open(path,O_RDWR|O_NONBLOCK|O_CLOEXEC); if(fd<0) continue;
        struct input_absinfo ax, ay;
        if(ioctl(fd,EVIOCGABS(ABS_MT_POSITION_X),&ax)==0 && ioctl(fd,EVIOCGABS(ABS_MT_POSITION_Y),&ay)==0 && ax.maximum>0){
            touch_fd=fd; touch_xmax=ax.maximum; touch_ymax=ay.maximum;
            L("[pid %d] touch device %s (%dx%d)\n",getpid(),path,touch_xmax,touch_ymax);
            return;
        }
        close(fd);
    }
    L("[pid %d] no touch device found\n",getpid());
}
static void ev(int type,int code,int value){
    struct input_event e; memset(&e,0,sizeof e); e.type=type; e.code=code; e.value=value;
    ssize_t r=write(touch_fd,&e,sizeof e); (void)r;
}
static void touch_tap_now(int sx,int sy){
    touch_open(); if(touch_fd<0) return;
    int rx = touch_xmax - sy*touch_xmax/800, ry = sx*touch_ymax/1280;
    if(rx<0) rx=0; if(rx>touch_xmax) rx=touch_xmax; if(ry<0) ry=0; if(ry>touch_ymax) ry=touch_ymax;
    ev(EV_ABS,ABS_MT_TRACKING_ID,touch_id++); ev(EV_ABS,ABS_MT_POSITION_X,rx); ev(EV_ABS,ABS_MT_POSITION_Y,ry);
    ev(EV_KEY,BTN_TOUCH,1); ev(EV_ABS,ABS_X,rx); ev(EV_ABS,ABS_Y,ry); ev(EV_SYN,SYN_REPORT,0);
    usleep(touch_ms*1000);
    ev(EV_ABS,ABS_MT_TRACKING_ID,-1); ev(EV_KEY,BTN_TOUCH,0); ev(EV_SYN,SYN_REPORT,0);
    L("[pid %d] TOUCH %d,%d (raw %d,%d)\n",getpid(),sx,sy,rx,ry);
}
static void* touch_thread(void*arg){
    int sx=((int*)arg)[0], sy=((int*)arg)[1]; free(arg);
    touch_tap_now(sx,sy);
    return NULL;
}
static void touch_tap(int x,int y){
    int*a=malloc(2*sizeof(int)); if(!a) return; a[0]=x; a[1]=y;
    pthread_t th; if(pthread_create(&th,NULL,touch_thread,a)==0) pthread_detach(th); else free(a);
}

/* The surface bytes of a note / note2 / menupad action. */
static int action_bytes(int mode,int arg,unsigned char*o){
    int w=0;
    if(mode==M_NOTE || mode==M_NOTE2){
        for(int r=0;r<(mode==M_NOTE2?2:1);r++){ o[w++]=0x90; o[w++]=(unsigned char)arg; o[w++]=0x7f; o[w++]=0x90; o[w++]=(unsigned char)arg; o[w++]=0x00; }
    } else if(mode==M_MENUPAD){
        o[w++]=0x90; o[w++]=MENU_CODE; o[w++]=0x7f; o[w++]=0x99; o[w++]=(unsigned char)arg; o[w++]=0x7f;
        o[w++]=0x89; o[w++]=(unsigned char)arg; o[w++]=0x40; o[w++]=0x90; o[w++]=MENU_CODE; o[w++]=0x00;
    }
    return w;
}
/* macro: the steps run in their own thread, in order (surface steps through the queue, touch taps directly) */
static void* macro_thread(void*arg){
    char*steps=arg, *save=NULL;
    for(char*tok=strtok_r(steps,"/",&save); tok; tok=strtok_r(NULL,"/",&save)){
        unsigned char b[24]; int n=0; long v=strtol(tok+1,NULL,0);
        switch(tok[0]){
            case 'b': b[n++]=0x90; b[n++]=(unsigned char)v; b[n++]=0x7f; b[n++]=0x90; b[n++]=(unsigned char)v; b[n++]=0x00; break;
            case 'p': b[n++]=0x99; b[n++]=(unsigned char)v; b[n++]=0x7f; b[n++]=0x89; b[n++]=(unsigned char)v; b[n++]=0x40; break;
            case 'm': n=action_bytes(M_MENUPAD,(int)v,b); break;
            case 't': { int x=0,y=0; sscanf(tok+1,"%d,%d",&x,&y); touch_tap_now(x,y); break; }
            case 'w': usleep((useconds_t)v*1000); break;
        }
        if(n) enqueue(b,n);
    }
    L("[pid %d] MACRO done\n",getpid());
    free(steps);
    return NULL;
}
static void run_macro(const char*steps){
    char*copy=strdup(steps); if(!copy) return;
    pthread_t th; if(pthread_create(&th,NULL,macro_thread,copy)==0) pthread_detach(th); else free(copy);
}

/* Run an action: CC modes out of the Padbangers port, touch on the touchscreen, surface actions into out (inside the
   read hook) or the queue (from a timer). */
static void run_action(int mode,int arg,int x,int y,const char*str,unsigned char*out,ssize_t*w,ssize_t cap){
    if(mode==M_MACRO){ run_macro(str); return; }
    if(mode==M_RELP || mode==M_RELM || mode==M_BTN){ fire(mode,arg); return; }
    if(mode==M_TOUCH){ touch_tap(x,y); return; }
    unsigned char b[24]; int n=action_bytes(mode,arg,b);
    if(n<=0) return;
    if(out && *w+n<=cap){ memcpy(out+*w,b,n); *w+=n; }
    else enqueue(b,n);
}

/* worker: fires the delayed single-click once the double-click window has elapsed */
static void* worker(void*arg){
    (void)arg;
    for(;;){
        usleep(20000);
        double now=now_ms();
        pthread_mutex_lock(&lk);
        for(int i=0;i<nmaps;i++){
            if(maps[i].armed && (now-maps[i].t_ms)>=dclick_ms){
                maps[i].armed=0; int m=maps[i].s_mode, c=maps[i].s_cc, x=maps[i].s_x, y=maps[i].s_y;
                char str[96]; memcpy(str,maps[i].s_str,sizeof str);
                pthread_mutex_unlock(&lk);
                run_action(m,c,x,y,str,NULL,NULL,0); L("[pid %d] SINGLE note0x%02x cc%d\n",getpid(),maps[i].note,c);
                pthread_mutex_lock(&lk);
            }
            if(maps[i].has_long && maps[i].held && !maps[i].long_fired && (now-maps[i].down_t)>=long_ms){
                maps[i].long_fired=1;
                int m=maps[i].l_mode, c=maps[i].l_cc, x=maps[i].l_x, y=maps[i].l_y, nt=maps[i].note;
                char str[96]; memcpy(str,maps[i].l_str,sizeof str);
                pthread_mutex_unlock(&lk);
                run_action(m,c,x,y,str,NULL,NULL,0); L("[pid %d] LONG note0x%02x\n",getpid(),nt);
                pthread_mutex_lock(&lk);
            }
        }
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

static mapping_t* find_map(int note,int shift){
    mapping_t*fb=NULL;
    for(int i=0;i<nmaps;i++) if(maps[i].note==note){
        if(shift && maps[i].is_shift) return &maps[i];   /* Shift held + shift-variant wins */
        if(!maps[i].is_shift) fb=&maps[i];               /* normal mapping as fallback */
    }
    return fb;
}

static void handle_press(mapping_t*m){
    if(m->s_mode==M_CPROBE){ int c=probe_cur; send_cc(c,1);
        L("[pid %d] CPROBE -> target %d (rel +1)\n",getpid(),c); probe_cur++; if(probe_cur>probe_end) probe_cur=probe_start; return; }
    if(m->s_mode==M_PROBE){ int c=probe_cur; send_cc(c,127); usleep(15000); send_cc(c,0);
        L("[pid %d] PROBE -> target %d\n",getpid(),c); probe_cur++; if(probe_cur>probe_end) probe_cur=probe_start; return; }
    if(!m->has_d){ fire(m->s_mode,m->s_cc); L("[pid %d] PRESS note0x%02x cc%d\n",getpid(),m->note,m->s_cc); return; }
    pthread_mutex_lock(&lk);
    double now=now_ms();
    if(m->armed && (now-m->t_ms)<dclick_ms){
        m->armed=0; int dm=m->d_mode, dc=m->d_cc;
        pthread_mutex_unlock(&lk);
        fire(dm,dc); L("[pid %d] DOUBLE note0x%02x cc%d\n",getpid(),m->note,dc);
    } else {
        m->armed=1; m->t_ms=now;
        pthread_mutex_unlock(&lk);
    }
}

/* Rebuild the surface stream: drop mapped buttons; for note/note2 modes inject the
   target button's note (1x / 2x) right here (synchronous with the triggering read);
   for cc modes fire the Padbangers CC via handle_press. */
/* A short press of a mapping: its single rule (double rule on a fast second press), or the button itself when the
   mapping only has a long rule. CC rules keep v9's delayed single (handle_press). */
static void short_press(mapping_t*m,int src,unsigned char*out,ssize_t*w,ssize_t cap){
    if(m->s_mode==M_NONE){ unsigned char o[6]={0x90,(unsigned char)src,0x7f,0x90,(unsigned char)src,0x00};
        if(*w+6<=cap){ memcpy(out+*w,o,6); *w+=6; } return; }
    if(m->s_mode==M_RELP || m->s_mode==M_RELM || m->s_mode==M_BTN){ handle_press(m); return; }
    if(m->has_d && (m->s_mode==M_MACRO || m->s_mode==M_TOUCH)){
        /* a macro must not start before it is clear whether a second press follows (the two would overlap): like the
           CC rules, the single waits out the double-click window (worker), a second press runs the double instead */
        pthread_mutex_lock(&lk);
        double now=now_ms();
        if(m->armed && (now-m->t_ms)<dclick_ms){
            m->armed=0; int dm=m->d_mode, dc=m->d_cc, dx=m->d_x, dy=m->d_y; char ds[96]; memcpy(ds,m->d_str,sizeof ds);
            pthread_mutex_unlock(&lk);
            run_action(dm,dc,dx,dy,ds,out,w,cap); L("[pid %d] DOUBLE src0x%02x\n",getpid(),src);
        } else { m->armed=1; m->t_ms=now; pthread_mutex_unlock(&lk); }
        return;
    }
    int mode=m->s_mode, arg=m->s_cc, x=m->s_x, y=m->s_y; const char*str=m->s_str;
    if(m->has_d){ double now=now_ms();
        if((now-m->t_ms)<dclick_ms){ mode=m->d_mode; arg=m->d_cc; x=m->d_x; y=m->d_y; str=m->d_str; }
        m->t_ms=now; }
    run_action(mode,arg,x,y,str,out,w,cap);
    L("[pid %d] SHORT src0x%02x mode %d\n",getpid(),src,mode);
}

static ssize_t filter(unsigned char*b, ssize_t n, size_t size){
    unsigned char out[512]; ssize_t w=0;
    if(n>0) maybe_reload();
    ssize_t cap = (ssize_t)(size < sizeof(out) ? size : sizeof(out));
    int pb = pending_bank;
    if(pb>=0 && w+6<=cap){
        /* bank lock: the user's bank goes in before this event, so a pad hit already plays in it */
        out[w++]=0x90; out[w++]=(unsigned char)pb; out[w++]=0x7f;
        out[w++]=0x90; out[w++]=(unsigned char)pb; out[w++]=0x00;
        pending_bank=-1; last_bank_inject=now_ms();
        L("[pid %d] BANKLOCK inject bank 0x%02x\n",getpid(),pb);
    }
    if(injq_n>0) w+=drain_queue(out+w,(int)(cap-w));   /* timer actions (long press) */
    for(ssize_t i=0;i<n;){
        if(i+2<n && (b[i]==0x90||b[i]==0x80)){
          if(b[i+1]==shift_note){
            shift_held = (b[i]==0x90 && b[i+2]!=0x00);
            if(shift_suppress){ i+=3; continue; }        /* hide Shift from the app */
          } else {
            mapping_t*m=find_map(b[i+1], shift_held);
            if(m && m->has_long){
                /* long rule: the press only starts the timer (worker); the release decides */
                if(b[i]==0x90 && b[i+2]!=0x00){
                    pthread_mutex_lock(&lk); m->held=1; m->long_fired=0; m->down_t=now_ms(); pthread_mutex_unlock(&lk);
                } else if(m->held){
                    pthread_mutex_lock(&lk); m->held=0; int fired=m->long_fired; pthread_mutex_unlock(&lk);
                    if(!fired) short_press(m,b[i+1],out,&w,cap);
                }
                i+=3; continue;
            }
            if(m){
                if(b[i]==0x90 && b[i+2]!=0x00 && (m->s_mode==M_TOUCH || m->s_mode==M_MACRO)){
                    short_press(m,b[i+1],out,&w,cap);
                } else if(b[i]==0x90 && b[i+2]!=0x00){
                    if(m->is_shift && !shift_suppress && w+3<=cap){
                        /* blip Shift OFF (app processes this surface event before our
                           Padbangers CC) so the app's global mapping accepts the CC;
                           Shift itself is still passed through normally otherwise, so
                           native Shift+button combos keep working. */
                        out[w++]=0x90; out[w++]=(unsigned char)shift_note; out[w++]=0x00;
                    }
                    if(m->s_mode==M_NOTE || m->s_mode==M_NOTE2){
                        /* immediate click-count: 1st press = single action, a fast
                           repeat = double action (both injected right here, no delay).
                           On a real double-click the single screen flashes then the
                           double screen wins. */
                        int mode=m->s_mode, target=m->s_cc;
                        if(m->has_d && (m->d_mode==M_NOTE || m->d_mode==M_NOTE2)){
                            double now=now_ms();
                            if((now - m->t_ms) < dclick_ms){ mode=m->d_mode; target=m->d_cc; }
                            m->t_ms=now;
                        }
                        int reps=(mode==M_NOTE2)?2:1;
                        for(int r=0;r<reps;r++) if(w+6<=cap){
                            out[w++]=0x90; out[w++]=(unsigned char)target; out[w++]=0x7f;
                            out[w++]=0x90; out[w++]=(unsigned char)target; out[w++]=0x00;
                        }
                        L("[pid %d] NOTEINJ src0x%02x -> note %d x%d\n",getpid(),b[i+1],target,reps);
                    } else if(m->s_mode==M_NPROBE){
                        int code=nprobe_cur;
                        /* first go back to Main (52) so each code is shown from a clean state */
                        if(w+12<=cap){
                            out[w++]=0x90; out[w++]=52; out[w++]=0x7f; out[w++]=0x90; out[w++]=52; out[w++]=0x00;
                            out[w++]=0x90; out[w++]=(unsigned char)code; out[w++]=0x7f;
                            out[w++]=0x90; out[w++]=(unsigned char)code; out[w++]=0x00;
                        }
                        L("[pid %d] NPROBE -> Main then code %d (0x%02x)\n",getpid(),code,code);
                        nprobe_cur++; if(nprobe_cur>nprobe_end) nprobe_cur=nprobe_start;
                    } else if(m->s_mode==M_MENUPAD){
                        int pad=m->s_cc;
                        if(m->has_d && m->d_mode==M_MENUPAD){
                            double now=now_ms();
                            if((now - m->t_ms) < dclick_ms) pad=m->d_cc;
                            m->t_ms=now;
                        }
                        if(w+12<=cap){
                            out[w++]=0x90; out[w++]=MENU_CODE; out[w++]=0x7f;      /* Menu down */
                            out[w++]=0x99; out[w++]=(unsigned char)pad; out[w++]=0x7f; /* Pad down (ch9) */
                            out[w++]=0x89; out[w++]=(unsigned char)pad; out[w++]=0x40; /* Pad up */
                            out[w++]=0x90; out[w++]=MENU_CODE; out[w++]=0x00;      /* Menu up */
                        }
                        L("[pid %d] MENUPAD src0x%02x -> Menu+pad %d\n",getpid(),b[i+1],pad);
                    } else {
                        handle_press(m);
                    }
                }
                i+=3; continue;   /* suppress source note (press + release) */
            } else if(b[i]==0x90 && b[i+2]!=0x00){
                if(b[i+1]>=BANK_A && b[i+1]<=BANK_D){
                    /* the user picks a bank: A-D becomes the locked bank, Shift+bank (E-H) suspends the lock */
                    if(shift_held){ bank_suspended=1; }
                    else { user_bank=b[i+1]; bank_suspended=0; }
                    last_bank_press=now_ms(); pending_bank=-1;
                }
                L("[pid %d] UNMAPPED note 0x%02x (%d)\n",getpid(),b[i+1],b[i+1]);
            }
          }
        }
        if(w<cap) out[w++]=b[i];
        i++;
    }
    memcpy(b,out,w);
    return w;
}

ssize_t snd_rawmidi_read(snd_rawmidi_t *rmidi, void *buffer, size_t size){
    if(!orig_rmread) orig_rmread=dlsym(RTLD_NEXT,"snd_rawmidi_read");
    if(!orig_poll) orig_poll=dlsym(RTLD_NEXT,"poll");
    if(active && (void*)rmidi==surface_in && has_pending() && surface_fd>=0){
        /* a correction is waiting: don't block in the real read if the surface has nothing (woken by wake_pipe) */
        struct pollfd q={ surface_fd, POLLIN, 0 };
        if(orig_poll(&q,1,0)<=0){ ssize_t m=filter((unsigned char*)buffer,0,size); return m>0?m:-11; }
    }
    ssize_t n=orig_rmread(rmidi,buffer,size);
    if(active && n>0 && (void*)rmidi==surface_in){ ssize_t m=filter((unsigned char*)buffer,n,size); if(m==0) return -11; return m; }
    if(active && n<0 && (void*)rmidi==surface_in && has_pending()){ ssize_t m=filter((unsigned char*)buffer,0,size); if(m>0) return m; }
    return n;
}
/* Bank lock: watch the bank LEDs the app writes to the controller (CC 0x23..0x26 on channel 1, value != 0 = lit).
   A real MIDI parser: the app uses running status (b0 23 00 25 03 = A off, C on), so a bank's CC need not follow a
   b0 byte (v7 missed those: Pad Perform kept bank C). The state carries over between writes. */
static void bank_led(int cc, int val){
    static int last_lit = -1;
    if(val==0) return;
    if(cc!=last_lit){ L("[pid %d] LED bank 0x%02x lit (user 0x%02x%s)\n",getpid(),cc,user_bank,bank_suspended?", suspended":""); last_lit=cc; }
    if(!banklock || bank_suspended || cc==user_bank) return;
    double now=now_ms();
    /* only the user's own presses are guarded: a correction can't loop, since it waits for the next surface event */
    if(now-last_bank_press>400 && pending_bank<0){
        pending_bank=user_bank;
        if(wake_pipe[1]>=0){ ssize_t r=write(wake_pipe[1],"x",1); (void)r; }
        L("[pid %d] BANKLOCK app lit bank 0x%02x, restoring 0x%02x on the next surface event\n",getpid(),cc,user_bank);
    }
}
ssize_t snd_rawmidi_write(snd_rawmidi_t *rmidi, const void *buffer, size_t size){
    static int status=0, need=0, have=0, d0=0, in_sysex=0;
    if(!orig_rmwrite) orig_rmwrite=dlsym(RTLD_NEXT,"snd_rawmidi_write");
    if(active && banklock && buffer && (void*)rmidi!=(void*)surface_in){
        const unsigned char*p=(const unsigned char*)buffer;
        for(size_t i=0;i<size;i++){
            unsigned char c=p[i];
            if(c>=0xf8) continue;                                   /* realtime: doesn't touch running status */
            if(c==0xf0){ in_sysex=1; status=0; continue; }
            if(c==0xf7){ in_sysex=0; continue; }
            if(in_sysex) continue;
            if(c>=0x80){
                if(c>=0xf0){ status=0; continue; }                 /* other system messages: drop the status */
                status=c; have=0; need=((c&0xf0)==0xc0 || (c&0xf0)==0xd0)?1:2; continue;
            }
            if(!status) continue;
            if(have==0){ d0=c; have=1; if(need==1) have=0; continue; }
            have=0;
            if(status==0xb0 && d0>=BANK_A && d0<=BANK_D) bank_led(d0,c);
        }
    }
    return orig_rmwrite(rmidi,buffer,size);
}

/* poll(): only a poll that watches the surface fd is touched; it also watches wake_pipe (see above). */
int poll(struct pollfd *fds, nfds_t nfds, int timeout){
    if(!orig_poll) orig_poll=dlsym(RTLD_NEXT,"poll");
    if(!active || surface_fd<0 || wake_pipe[0]<0 || nfds==0 || nfds>15) return orig_poll(fds,nfds,timeout);
    int si=-1;
    for(nfds_t i=0;i<nfds;i++) if(fds[i].fd==surface_fd && (fds[i].events&POLLIN)){ si=(int)i; break; }
    if(si<0) return orig_poll(fds,nfds,timeout);
    if(!poll_seen){ poll_seen=1; L("[pid %d] poll on the surface fd (%d fds)\n",getpid(),(int)nfds); }
    if(has_pending()){ for(nfds_t i=0;i<nfds;i++) fds[i].revents=0; fds[si].revents=POLLIN; return 1; }
    struct pollfd all[16];
    memcpy(all,fds,nfds*sizeof *fds);
    all[nfds].fd=wake_pipe[0]; all[nfds].events=POLLIN; all[nfds].revents=0;
    int r=orig_poll(all,nfds+1,timeout);
    if(r<0) return r;
    if(all[nfds].revents){ char tmp[16]; while(read(wake_pipe[0],tmp,sizeof tmp)>0){} }
    int cnt=0;
    for(nfds_t i=0;i<nfds;i++){ fds[i].revents=all[i].revents; }
    if(all[nfds].revents && has_pending()) fds[si].revents|=POLLIN;
    for(nfds_t i=0;i<nfds;i++) if(fds[i].revents) cnt++;
    return cnt;
}

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode){
    if(!orig_rmopen) orig_rmopen=dlsym(RTLD_NEXT,"snd_rawmidi_open");
    int r=orig_rmopen(in,out,name,mode);
    if(active && r==0 && name && in && *in && strstr(name,"2,0,1")){
        surface_in=*in;
        int (*pd)(snd_rawmidi_t*,struct pollfd*,unsigned int)=dlsym(RTLD_DEFAULT,"snd_rawmidi_poll_descriptors");
        struct pollfd q; if(pd && pd(*in,&q,1)==1) surface_fd=q.fd;
        if(wake_pipe[0]<0 && pipe(wake_pipe)==0){
            fcntl(wake_pipe[0],F_SETFL,O_NONBLOCK); fcntl(wake_pipe[1],F_SETFL,O_NONBLOCK);
            fcntl(wake_pipe[0],F_SETFD,FD_CLOEXEC); fcntl(wake_pipe[1],F_SETFD,FD_CLOEXEC);
        }
        L("[pid %d] surface_in set (fd %d, mode 0x%x, wake pipe %d)\n",getpid(),surface_fd,mode,wake_pipe[0]);
    }
    return r;
}

__attribute__((constructor))
static void init(void){
    /* Loaded globally via /etc/ld.so.preload -> stay COMPLETELY inert unless we are
       the MPC app process. No log, no ALSA calls, no thread in any other process. */
    char comm[64]="?"; FILE*f=fopen("/proc/self/comm","r"); if(f){if(fgets(comm,sizeof comm,f)){char*n=strchr(comm,'\n');if(n)*n=0;}fclose(f);}
    if(strcmp(comm,"MPC")!=0) return;
    active=1;
    orig_rmopen=dlsym(RTLD_NEXT,"snd_rawmidi_open");
    orig_rmread=dlsym(RTLD_NEXT,"snd_rawmidi_read");
    orig_rmwrite=dlsym(RTLD_NEXT,"snd_rawmidi_write");
    orig_seqopen=dlsym(RTLD_NEXT,"snd_seq_open");
    orig_clientid=dlsym(RTLD_NEXT,"snd_seq_client_id");
    orig_setname=dlsym(RTLD_NEXT,"snd_seq_set_client_name");
    orig_createport=dlsym(RTLD_NEXT,"snd_seq_create_simple_port");
    orig_output=dlsym(RTLD_NEXT,"snd_seq_event_output");
    orig_drain=dlsym(RTLD_NEXT,"snd_seq_drain_output");
    char*e;
    if((e=getenv("REMAP_CH"))) inj_ch=atoi(e);
    if((e=getenv("REMAP_PORTNAME"))) portname=e;
    if((e=getenv("REMAP_CONF"))) confpath=e;
    if((e=getenv("REMAP_DCLICK_MS"))) dclick_ms=atoi(e);
    L("=== [pid %d comm=%s] shim_remap10 dclick=%dms ===\n", getpid(), comm, dclick_ms);
    load_conf(); make_vport(); pthread_t th; pthread_create(&th,NULL,worker,NULL);
}
