/* arec - minimal ALSA capture -> WAV (stereo S16 from the first 2 channels).
 * Test tool: checks whether the MPC audio card's capture endpoint is openable
 * (concurrently with the app) and contains the output. Links device libasound.
 *
 * RESULT on this MPC X (2026-08-07): open fails with "Device or resource busy" --
 * the MPC app holds card1 pcm0c EXCLUSIVELY, and that capture is the 4ch physical
 * INPUTS (not the music). The 8ch music OUTPUT (playback) cannot be ALSA-captured.
 * => the Akai-Force "ffmpeg -f alsa -i hw:2" trick does NOT work here. See audio/README.
 *
 * Build (from the scratchpad with the device libs):
 *   zig cc -target arm-linux-gnueabihf.2.31 -O2 -o arec arec.c ./libasound.so.2 \
 *       -Wl,--allow-shlib-undefined
 * Usage: arec <out.wav> <seconds> [device hw:1,0] [channels 4] [rate 44100]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
extern int snd_pcm_open(snd_pcm_t**, const char*, int, int);
extern int snd_pcm_set_params(snd_pcm_t*, int format, int access, unsigned int channels,
                              unsigned int rate, int soft_resample, unsigned int latency);
extern snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t*, void*, snd_pcm_uframes_t);
extern int snd_pcm_recover(snd_pcm_t*, int err, int silent);
extern int snd_pcm_close(snd_pcm_t*);
extern const char* snd_strerror(int);
#define SND_PCM_STREAM_CAPTURE 1
#define SND_PCM_FORMAT_S32_LE 10
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

static void w32(FILE*f,uint32_t v){fputc(v,f);fputc(v>>8,f);fputc(v>>16,f);fputc(v>>24,f);}
static void w16(FILE*f,uint16_t v){fputc(v,f);fputc(v>>8,f);}

int main(int argc,char**argv){
    if(argc<3){fprintf(stderr,"usage: %s out.wav seconds [dev] [ch] [rate]\n",argv[0]);return 2;}
    const char*out=argv[1]; int secs=atoi(argv[2]);
    const char*dev=argc>3?argv[3]:"hw:1,0"; int ch=argc>4?atoi(argv[4]):4; int rate=argc>5?atoi(argv[5]):44100;
    snd_pcm_t*pcm; int e;
    e=snd_pcm_open(&pcm,dev,SND_PCM_STREAM_CAPTURE,0);
    if(e<0){fprintf(stderr,"open %s: %s\n",dev,snd_strerror(e));return 1;}
    e=snd_pcm_set_params(pcm,SND_PCM_FORMAT_S32_LE,SND_PCM_ACCESS_RW_INTERLEAVED,ch,rate,1,500000);
    if(e<0){fprintf(stderr,"set_params: %s\n",snd_strerror(e));return 1;}
    fprintf(stderr,"opened %s ok (%dch %dHz S32)\n",dev,ch,rate);

    int frames=rate*secs, per=256;
    int32_t*buf=malloc((size_t)per*ch*sizeof(int32_t));
    int16_t*st=malloc((size_t)frames*2*sizeof(int16_t));
    long got=0; double sum=0; long peak=0;
    while(got<frames){
        snd_pcm_sframes_t r=snd_pcm_readi(pcm,buf,per);
        if(r<0){ r=snd_pcm_recover(pcm,r,1); if(r<0){fprintf(stderr,"read: %s\n",snd_strerror((int)r));break;} continue; }
        for(int i=0;i<r && got<frames;i++){
            int32_t L=buf[i*ch+0], R=buf[i*ch+(ch>1?1:0)];
            int16_t l=(int16_t)(L>>16), rr=(int16_t)(R>>16);
            st[got*2]=l; st[got*2+1]=rr; got++;
            long a=l<0?-l:l; if(a>peak)peak=a; sum+=(double)a;
        }
    }
    snd_pcm_close(pcm);

    FILE*f=fopen(out,"wb"); if(!f){perror("out");return 1;}
    uint32_t data=(uint32_t)got*4; fwrite("RIFF",1,4,f); w32(f,36+data); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(f,16); w16(f,1); w16(f,2); w32(f,rate); w32(f,rate*4); w16(f,4); w16(f,16);
    fwrite("data",1,4,f); w32(f,data); fwrite(st,1,data,f); fclose(f);
    fprintf(stderr,"wrote %s: %ld frames, peak=%ld avg=%.0f (%s)\n",out,got,peak,got?sum/got:0,
            peak>50?"HAT SIGNAL":"still/leise");
    return 0;
}
