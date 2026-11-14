/* shim_remap7.so - config-driven MPC surface-button remapper with single/double-click, plus a pad bank lock.
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

enum { M_RELP=0, M_RELM=1, M_BTN=2, M_PROBE=3, M_NOTE=4, M_NOTE2=5, M_NPROBE=6, M_MENUPAD=7, M_CPROBE=8, M_NONE=-1 };
#define MENU_CODE 123
static int probe_cur=-1, probe_start=90, probe_end=118;
static int nprobe_cur=0, nprobe_start=0, nprobe_end=127;
typedef struct {
    int note;
    int is_shift;                 /* 1 = only active while Shift is held */
    int s_mode, s_cc;
    int has_d, d_mode, d_cc;
    int armed; double t_ms;
} mapping_t;
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
static ssize_t (*orig_rmwrite)(snd_rawmidi_t*, const void*, size_t) = NULL;
static int (*orig_rmopen)(snd_rawmidi_t**, snd_rawmidi_t**, const char*, int) = NULL;
static ssize_t (*orig_rmread)(snd_rawmidi_t*, void*, size_t) = NULL;
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
static int parse_mode(const char*s){ return !strcmp(s,"rel+")?M_RELP : !strcmp(s,"rel-")?M_RELM : !strcmp(s,"btn")?M_BTN : !strcmp(s,"probe")?M_PROBE : !strcmp(s,"note")?M_NOTE : !strcmp(s,"note2")?M_NOTE2 : !strcmp(s,"nprobe")?M_NPROBE : !strcmp(s,"menupad")?M_MENUPAD : !strcmp(s,"cprobe")?M_CPROBE : M_NONE; }

static void load_conf(void){
    nmaps=0; FILE*f=fopen(confpath,"r"); if(!f){ L("conf %s not found\n",confpath); return; }
    char line[160];
    while(fgets(line,sizeof line,f)){
        char*p=line; while(*p==' '||*p=='\t')p++;
        if(*p=='#'||*p=='\n'||*p==0) continue;
        char tok[16]={0},sm[16]={0},dm[16]={0}; int scc=0,dcc=0;
        int k=sscanf(p,"%15s %15s %d %15s %d",tok,sm,&scc,dm,&dcc);
        if(k>=2 && !strcmp(tok,"shift")){ shift_note=(int)strtol(sm,0,16); L("conf: shift note = 0x%02x\n",shift_note); continue; }
        if(k>=2 && !strcmp(tok,"shiftsuppress")){ shift_suppress=atoi(sm); L("conf: shiftsuppress = %d\n",shift_suppress); continue; }
        if(k>=2 && !strcmp(tok,"banklock")){ banklock=atoi(sm); L("conf: banklock = %d\n",banklock); continue; }
        if(k>=3 && nmaps<128){
            int is_shift=(tok[0]=='s'||tok[0]=='S');
            int note=(int)strtol(is_shift?tok+1:tok,0,16);
            int m1=parse_mode(sm); if(m1==M_NONE){ L("conf bad mode %s\n",sm); continue; }
            mapping_t*mp=&maps[nmaps];
            mp->note=note; mp->is_shift=is_shift; mp->s_mode=m1; mp->s_cc=scc; mp->armed=0; mp->has_d=0;
            if(m1==M_PROBE){ probe_start=scc; probe_cur=scc; }
            if(m1==M_NPROBE){ nprobe_start=scc; nprobe_cur=scc; }
            if(m1==M_CPROBE){ probe_start=scc; probe_cur=scc; probe_end=89; }
            if(k>=5){ int m2=parse_mode(dm); if(m2!=M_NONE){ mp->has_d=1; mp->d_mode=m2; mp->d_cc=dcc; } }
            nmaps++;
            L("conf: %snote 0x%02x single(%s cc%d)%s\n", is_shift?"SHIFT+":"", note, sm, scc, mp->has_d?" +double":"");
        }
    }
    fclose(f); L("conf: %d mapping(s)\n", nmaps);
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

/* worker: fires the delayed single-click once the double-click window has elapsed */
static void* worker(void*arg){
    (void)arg;
    for(;;){
        usleep(20000);
        double now=now_ms();
        pthread_mutex_lock(&lk);
        for(int i=0;i<nmaps;i++){
            if(maps[i].armed && (now-maps[i].t_ms)>=dclick_ms){
                maps[i].armed=0; int m=maps[i].s_mode, c=maps[i].s_cc;
                pthread_mutex_unlock(&lk);
                fire(m,c); L("[pid %d] SINGLE note0x%02x cc%d\n",getpid(),maps[i].note,c);
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
static ssize_t filter(unsigned char*b, ssize_t n, size_t size){
    unsigned char out[256]; ssize_t w=0;
    ssize_t cap = (ssize_t)(size < sizeof(out) ? size : sizeof(out));
    int pb = pending_bank;
    if(pb>=0 && w+6<=cap){
        /* bank lock: the user's bank goes in before this event, so a pad hit already plays in it */
        out[w++]=0x90; out[w++]=(unsigned char)pb; out[w++]=0x7f;
        out[w++]=0x90; out[w++]=(unsigned char)pb; out[w++]=0x00;
        pending_bank=-1; last_bank_inject=now_ms();
        L("[pid %d] BANKLOCK inject bank 0x%02x\n",getpid(),pb);
    }
    for(ssize_t i=0;i<n;){
        if(i+2<n && (b[i]==0x90||b[i]==0x80)){
          if(b[i+1]==shift_note){
            shift_held = (b[i]==0x90 && b[i+2]!=0x00);
            if(shift_suppress){ i+=3; continue; }        /* hide Shift from the app */
          } else {
            mapping_t*m=find_map(b[i+1], shift_held);
            if(m){
                if(b[i]==0x90 && b[i+2]!=0x00){
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
    ssize_t n=orig_rmread(rmidi,buffer,size);
    if(active && n>0 && (void*)rmidi==surface_in){ ssize_t m=filter((unsigned char*)buffer,n,size); if(m==0) return -11; return m; }
    return n;
}
/* Bank lock: watch the bank LEDs the app writes to the controller (b0 <0x23..0x26> <vv>, vv != 0 = lit). */
ssize_t snd_rawmidi_write(snd_rawmidi_t *rmidi, const void *buffer, size_t size){
    if(!orig_rmwrite) orig_rmwrite=dlsym(RTLD_NEXT,"snd_rawmidi_write");
    if(active && banklock && !bank_suspended && buffer){
        const unsigned char*p=(const unsigned char*)buffer;
        for(size_t i=0;i+2<size;i++){
            if(p[i]==0xb0 && p[i+1]>=BANK_A && p[i+1]<=BANK_D && p[i+2]!=0 && p[i+1]!=user_bank){
                double now=now_ms();
                /* only the user's own presses are guarded: a correction can't loop, since it waits for the next
                   surface event (Pad Perform via double click lights C again right after a correction) */
                if(now-last_bank_press>400 && pending_bank<0){
                    pending_bank=user_bank;
                    L("[pid %d] BANKLOCK app lit bank 0x%02x, restoring 0x%02x on the next surface event\n",getpid(),p[i+1],user_bank);
                }
                break;
            }
        }
    }
    return orig_rmwrite(rmidi,buffer,size);
}

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode){
    if(!orig_rmopen) orig_rmopen=dlsym(RTLD_NEXT,"snd_rawmidi_open");
    int r=orig_rmopen(in,out,name,mode);
    if(active && r==0 && name && in && *in && strstr(name,"2,0,1")){ surface_in=*in; L("[pid %d] surface_in set\n",getpid()); }
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
    L("=== [pid %d comm=%s] shim_remap7 dclick=%dms ===\n", getpid(), comm, dclick_ms);
    load_conf(); make_vport(); pthread_t th; pthread_create(&th,NULL,worker,NULL);
}
