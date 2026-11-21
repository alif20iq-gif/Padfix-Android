// padfix.c - Padfix V10 (Android: Termux + shell/ADB/Shizuku)
// Koreksi analog saja; D-pad ABS_HAT diterjemahkan menjadi tombol BTN_DPAD virtual.
// screenshot (screencap) dan dashboard localhost. Backend harus jalan sebagai user
// shell (uid 2000) agar bisa akses /dev/input & /dev/uinput. Tanpa root.
// Build (Termux): clang -O2 -pthread -o padfix padfix.c
// Opsi: -e /dev/input/eventN  -n substring_nama  -c ms_kalibrasi  -m margin  -z dz_min
//       -k persen_sisi_terpotong  -w file_index.html  -d (hanya dashboard, tanpa gamepad)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define BPL (sizeof(unsigned long) * 8)
#define NB(x) ((((x) - 1) / BPL) + 1)
#define PORT 8765
#define SHOT "/sdcard/Pictures/Padfix/Padfix-latest.png"
#define RECORD_DIR "/sdcard/Movies/Padfix"

static volatile sig_atomic_t running = 1;
static volatile int cfg_margin = 600, cfg_min_dz = 1500, cfg_kp = 0;
// sens[axis*2+0/-1] = faktor jangkauan per arah; 50% memberi power 2x, 100% normal, 200% memberi power 0.5x.
static volatile int sens[8] = {100,100,100,100,100,100,100,100};
static volatile int stick_power[2] = {100,100};
// Offset manual per axis: koreksi titik tengah hasil kalibrasi.
static volatile int cfg_offset[4] = {0,0,0,0};
static volatile int dir_min[16] = {100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100};
static volatile int dir_max[16] = {100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100};
static volatile int live_raw[4], live_fixed[4], live_center[4], live_dz[4];
static volatile int live_cal = 0, live_conn = 0, live_grab = 0, want_recal = 0, last_key = 0;
static volatile int virtual_active = 0, live_state = 0; /* 0 waiting, 1 connected, 2 calibrating, 3 complete, 4 disconnected */
static unsigned char virtual_key[KEY_MAX + 1], virtual_abs[ABS_MAX + 1];
static unsigned char have_key[KEY_MAX + 1];
static char dev_name[128] = "";
static int ufd = -1;
static pthread_mutex_t um = PTHREAD_MUTEX_INITIALIZER;
static char token[17];
static const char *www = "/data/local/tmp/padfix_index.html";
static const char *match = "";
static const int AX[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
static int SRC_AX[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
static unsigned char pressed_key[KEY_MAX + 1];
static volatile int stick_offset[4];
static volatile int recording=0;
static pid_t recorder_pid=-1;
static char recording_path[256]="";
static char capture_message[96]="";
static int capture_down=0,capture_long_fired=0;
static int64_t capture_press_ms=0,capture_last_tap_ms=0;
static void capture_button_event(int value);
static void capture_button_tick(void);

static const char *config_path = "/data/local/tmp/padfix.conf";
static void save_config(void) {
    char tmp[96]; snprintf(tmp,sizeof tmp,"%s.tmp",config_path);
    FILE *f=fopen(tmp,"w"); if(!f)return;
    fprintf(f,"%d %d %d\n",cfg_margin,cfg_min_dz,cfg_kp);
    for(int i=0;i<8;i++)fprintf(f,"%d%c",sens[i],i==7?'\n':' ');
    for(int i=0;i<4;i++)fprintf(f,"%d%c",cfg_offset[i],i==3?'\n':' ');
    for(int i=0;i<16;i++)fprintf(f,"%d%c",dir_min[i],i==15?'\n':' ');
    for(int i=0;i<16;i++)fprintf(f,"%d%c",dir_max[i],i==15?'\n':' ');
    fprintf(f,"%d %d\n",stick_power[0],stick_power[1]);
    if(fclose(f)==0){if(rename(tmp,config_path)!=0)unlink(tmp);}else unlink(tmp);
}
static void load_config(void) {
    FILE *f=fopen(config_path,"r");if(!f)return;
    int m,z,k,s[8],o[4],dmin[16],dmax[16],pL=100,pR=100,ok=fscanf(f,"%d%d%d",&m,&z,&k)==3;
    for(int i=0;ok&&i<8;i++)ok=fscanf(f,"%d",&s[i])==1&&s[i]>=50&&s[i]<=200;
    for(int i=0;ok&&i<4;i++)ok=fscanf(f,"%d",&o[i])==1&&o[i]>=-30000&&o[i]<=30000;
    int have_dir=1; for(int i=0;i<16;i++) if(fscanf(f,"%d",&dmin[i])!=1||dmin[i]<50||dmin[i]>200) have_dir=0;
    for(int i=0;i<16;i++) if(have_dir&&(fscanf(f,"%d",&dmax[i])!=1||dmax[i]<50||dmax[i]>200)) have_dir=0;
    int have_power=fscanf(f,"%d%d",&pL,&pR)==2&&pL>=50&&pL<=200&&pR>=50&&pR<=200;
    fclose(f);
    if(ok&&m>=0&&m<=30000&&z>=0&&z<=30000&&k>=0&&k<=100){
        cfg_margin=m;cfg_min_dz=z;cfg_kp=k;
        for(int i=0;i<8;i++)sens[i]=s[i];
        for(int i=0;i<4;i++)cfg_offset[i]=o[i];
        if(have_dir){for(int i=0;i<16;i++){dir_min[i]=dmin[i];dir_max[i]=dmax[i];}}
        if(have_power){stick_power[0]=pL;stick_power[1]=pR;}
        printf("Konfigurasi terakhir dipulihkan.\n");
    }
}


static void on_sig(int s) { (void)s; running = 0; }
static int idx_of(int code) { for (int i = 0; i < 4; i++) if (SRC_AX[i] == code) return i; return -1; }
static int tbit(int b, const unsigned long *a) { return (a[b / BPL] >> (b % BPL)) & 1; }
static int64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int emit(int type, int code, int val) {
    struct input_event e; memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = val;
    int r = -1;
    pthread_mutex_lock(&um);
    if (ufd >= 0) r = write(ufd, &e, sizeof e) == (ssize_t)sizeof e ? 0 : -1;
    pthread_mutex_unlock(&um);
    return r;
}

// Kirim satu paket input sekaligus. Ini mengurangi syscall dan menghindari
// jeda antar-sumbu/tombol dalam satu frame gamepad.
static void emit_batch(const struct input_event *events, size_t count) {
    if (!count) return;
    pthread_mutex_lock(&um);
    if (ufd >= 0) {
        const unsigned char *p = (const unsigned char *)events;
        size_t left = count * sizeof(*events);
        while (left) {
            ssize_t n = write(ufd, p, left);
            if (n > 0) { p += n; left -= (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd po = {ufd, POLLOUT, 0};
                if (poll(&po, 1, 2) > 0 && (po.revents & POLLOUT)) continue;
            }
            break;
        }
    }
    pthread_mutex_unlock(&um);
}

static int find_pad(char *out, size_t n) {
    for (int i = 0; i < 64; i++) {
        char p[64], name[256] = {0};
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long keys[NB(KEY_MAX+1)], axes[NB(ABS_MAX+1)];
        memset(keys,0,sizeof keys); memset(axes,0,sizeof axes);
        int named=ioctl(fd,EVIOCGNAME(sizeof name),name)>0;
        ioctl(fd,EVIOCGBIT(EV_KEY,sizeof keys),keys);
        ioctl(fd,EVIOCGBIT(EV_ABS,sizeof axes),axes);
        int is_gamepad=tbit(ABS_X,axes)&&tbit(ABS_Y,axes) &&
            (tbit(BTN_GAMEPAD,keys)||tbit(BTN_JOYSTICK,keys)||tbit(BTN_A,keys)||
             tbit(BTN_SOUTH,keys)||tbit(BTN_TRIGGER,keys));
        int ok=named&&is_gamepad&&(!match[0]||strstr(name,match))&&
            !strstr(name,"IMU")&&!strstr(name,"PadFix Virtual Gamepad")&&!strstr(name,"(padfix)");
        close(fd);
        if(ok){snprintf(out,n,"%s",p);return 0;}
    }
    return -1;
}

// Hasil: offset bertanda dari titik tengah output, rentang -half..+half.
static int fix(int v, int c, int dz, int lo, int hi, int half, int kp) {
    int d = v - c, ad = d < 0 ? -d : d;
    if (ad <= dz) return 0;
    int lim = d > 0 ? hi - c : c - lo;
    if (lim > half) lim = half; else lim += (int)((long)(half - lim) * kp / 100);
    if (lim <= dz + 1) lim = dz + 2;
    long o = (long)(ad - dz) * half / (lim - dz);
    if (o > half) o = half;
    return d > 0 ? (int)o : (int)-o;
}

// ---------------- virtual device persisten + sesi sumber fisik ----------------
static int have_cal = 0, cen[4], noise[4];

static int create_virtual_device(void) {
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (u < 0) { perror("open /dev/uinput"); return -1; }
    if (ioctl(u, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(u, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(u, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("uinput event capabilities"); close(u); return -1;
    }
    memset(virtual_key, 0, sizeof virtual_key);
    memset(virtual_abs, 0, sizeof virtual_abs);
    /* Sediakan tombol gamepad umum sebelum controller fisik tersambung. */
    for (int k = BTN_MISC; k <= KEY_MAX; k++) {
        if (ioctl(u, UI_SET_KEYBIT, k) == 0) virtual_key[k] = 1;
    }
    const int keys[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
        BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR, BTN_DPAD_UP, BTN_DPAD_DOWN,
        BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
    for (size_t i=0; i<sizeof keys/sizeof keys[0]; i++) {
        if (ioctl(u, UI_SET_KEYBIT, keys[i]) == 0) virtual_key[keys[i]] = 1;
    }

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "PadFix Virtual Gamepad");
    ud.id.bustype = BUS_USB;
    ud.id.vendor = 0x1209;  /* PID/VID tetap: tidak berubah saat reconnect */
    ud.id.product = 0x5046;
    ud.id.version = 1;

    const int sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
    for (size_t i=0; i<sizeof sticks/sizeof sticks[0]; i++) {
        int a=sticks[i];
        if (ioctl(u, UI_SET_ABSBIT, a) < 0) { perror("uinput stick axis"); close(u); return -1; }
        virtual_abs[a]=1; ud.absmin[a]=-24000; ud.absmax[a]=24000;
        ud.absfuzz[a]=0; ud.absflat[a]=0;
    }
    const int triggers[] = {ABS_Z, ABS_RZ, ABS_GAS, ABS_BRAKE, ABS_THROTTLE, ABS_RUDDER};
    for (size_t i=0; i<sizeof triggers/sizeof triggers[0]; i++) {
        int a=triggers[i];
        if (ioctl(u, UI_SET_ABSBIT, a)==0) {
            virtual_abs[a]=1; ud.absmin[a]=0; ud.absmax[a]=255;
        }
    }
    const int hats[] = {ABS_HAT0X, ABS_HAT0Y, ABS_HAT1X, ABS_HAT1Y};
    for (size_t i=0; i<sizeof hats/sizeof hats[0]; i++) {
        int a=hats[i];
        if (ioctl(u, UI_SET_ABSBIT, a)==0) {
            virtual_abs[a]=1; ud.absmin[a]=-1; ud.absmax[a]=1;
        }
    }
    if (write(u, &ud, sizeof ud) != (ssize_t)sizeof ud || ioctl(u, UI_DEV_CREATE) < 0) {
        perror("uinput create persistent virtual gamepad"); close(u); return -1;
    }
    pthread_mutex_lock(&um);
    ufd=u;
    pthread_mutex_unlock(&um);
    virtual_active=1;
    printf("[VIRTUAL] VIRTUAL_DEVICE_ACTIVE: PadFix Virtual Gamepad dibuat sekali (VID:PID 1209:5046).\n");
    fflush(stdout);
    return 0;
}

static void release_virtual_inputs(void) {
    struct input_event out[KEY_MAX + 8]; size_t n=0;
    for (int k=0;k<=KEY_MAX;k++) if (pressed_key[k]) {
        memset(&out[n],0,sizeof out[n]); out[n].type=EV_KEY; out[n].code=k; out[n].value=0; n++;
        pressed_key[k]=0;
    }
    const int axes[]={ABS_X,ABS_Y,ABS_RX,ABS_RY};
    for(size_t i=0;i<4;i++){
        memset(&out[n],0,sizeof out[n]); out[n].type=EV_ABS; out[n].code=axes[i]; out[n].value=0; n++;
        live_fixed[i]=0; stick_offset[i]=0;
    }
    memset(&out[n],0,sizeof out[n]); out[n].type=EV_SYN; out[n].code=SYN_REPORT; n++;
    emit_batch(out,n);
}

static int scale_to_virtual(int value, int lo, int hi, int vmin, int vmax) {
    if (hi <= lo) return vmin;
    long scaled=(long)(value-lo)*(vmax-vmin)/(hi-lo)+vmin;
    if(scaled<vmin)scaled=vmin;
    if(scaled>vmax)scaled=vmax;
    return (int)scaled;
}

/* Satu sesi hanya membuka/membaca sumber fisik. Virtual uinput tidak disentuh. */
static void *r3_keepalive_main(void *unused) {
    (void)unused;
    int64_t next_pulse=now_ms()+10000;
    while(running){
        capture_button_tick();
        if(live_conn || !virtual_active){
            next_pulse=now_ms()+10000;
            struct timespec pause={0,200000000};
            nanosleep(&pause,NULL);
            continue;
        }
        int64_t now=now_ms();
        if(now<next_pulse){
            int wait=(int)(next_pulse-now);
            if(wait>200)wait=200;
            struct timespec pause={wait/1000,(wait%1000)*1000000L};
            nanosleep(&pause,NULL);
            continue;
        }
        if(!live_conn && virtual_active && running){
            struct input_event pulse[2];
            memset(pulse,0,sizeof pulse);
            pulse[0].type=EV_KEY;pulse[0].code=BTN_THUMBR;pulse[0].value=1;
            pulse[1].type=EV_SYN;pulse[1].code=SYN_REPORT;
            emit_batch(pulse,2);
            printf("[VIRTUAL] R3 keep-alive press (controller fisik belum terhubung).\\n");
            struct timespec hold={0,100000000L};
            nanosleep(&hold,NULL);
            memset(pulse,0,sizeof pulse);
            pulse[0].type=EV_KEY;pulse[0].code=BTN_THUMBR;pulse[0].value=0;
            pulse[1].type=EV_SYN;pulse[1].code=SYN_REPORT;
            emit_batch(pulse,2);
            printf("[VIRTUAL] R3 keep-alive release.\\n");
        }
        next_pulse=now_ms()+10000;
    }
    return NULL;
}

static int session(const char *fixed, int cal_ms, int *announced) {
    char path[64]={0};
    if(fixed[0]) snprintf(path,sizeof path,"%s",fixed);
    else if(find_pad(path,sizeof path)<0) {
        live_conn=0; live_grab=0; live_cal=0;
        if(live_state!=0) live_state=4;
        else live_state=0;
        if(!*announced){printf("[PHYSICAL] WAITING_FOR_PHYSICAL_GAMEPAD: menunggu controller fisik.\n");*announced=1;}
        return 1;
    }
    int fd=open(path,O_RDONLY|O_NONBLOCK);
    if(fd<0)return 1;

    unsigned long keybits[NB(KEY_MAX+1)],absbits[NB(ABS_MAX+1)];
    memset(keybits,0,sizeof keybits);memset(absbits,0,sizeof absbits);
    ioctl(fd,EVIOCGBIT(EV_KEY,sizeof keybits),keybits);
    ioctl(fd,EVIOCGBIT(EV_ABS,sizeof absbits),absbits);
    char rname[128]={0}; ioctl(fd,EVIOCGNAME(sizeof rname),rname);
    memset(have_key,0,sizeof have_key);
    for(int k=0;k<=KEY_MAX;k++)if(tbit(k,keybits))have_key[k]=1;
    have_key[BTN_DPAD_UP]=have_key[BTN_DPAD_DOWN]=1;
    have_key[BTN_DPAD_LEFT]=have_key[BTN_DPAD_RIGHT]=1;
    memset(pressed_key,0,sizeof pressed_key);
    int lo[4]={-32768,-32768,-32768,-32768};
    int hi[4]={32767,32767,32767,32767};
    int half[4]={32767,32767,32767,32767},oc[4]={0,0,0,0};
    SRC_AX[0]=ABS_X; SRC_AX[1]=ABS_Y;
    SRC_AX[2]=ABS_RX; SRC_AX[3]=ABS_RY;
    if(!(tbit(ABS_RX,absbits)&&tbit(ABS_RY,absbits)) && tbit(ABS_Z,absbits)&&tbit(ABS_RZ,absbits)){
        SRC_AX[2]=ABS_Z; SRC_AX[3]=ABS_RZ;
    }
    printf("[ANALOG] Axis mapping: L=%d/%d R=%d/%d -> virtual X/Y/RX/RY.\n",SRC_AX[0],SRC_AX[1],SRC_AX[2],SRC_AX[3]);
    for(int i=0;i<4;i++){
        struct input_absinfo ai;
        if(ioctl(fd,EVIOCGABS(SRC_AX[i]),&ai)==0){
            lo[i]=ai.minimum;hi[i]=ai.maximum;
            if(hi[i]<=lo[i]){lo[i]=-32768;hi[i]=32767;}
            half[i]=(hi[i]-lo[i])/2;oc[i]=(hi[i]+lo[i])/2;
        }
    }
    live_grab=(ioctl(fd,EVIOCGRAB,1)==0);
    if(!live_grab)perror("EVIOCGRAB (input fisik mungkin masih masuk ke aplikasi lain)");
    snprintf(dev_name,sizeof dev_name,"%s",rname);
    for(char *p=dev_name;*p;p++)if(*p=='"'||*p=='\\')*p='_';
    live_conn=1;live_state=2;live_cal=0;*announced=0;
    printf("[PHYSICAL] PHYSICAL_GAMEPAD_CONNECTED: %s (%s). Virtual device tetap sama.\n",rname,path);
    printf("[CALIBRATION] CALIBRATION_IN_PROGRESS: ambil sampel netral %d ms; abaikan input sampai selesai.\n",cal_ms);
    fflush(stdout);

    int mn[4],mx[4],last[4];
    for(int i=0;i<4;i++){
        struct input_absinfo ai;
        if(ioctl(fd,EVIOCGABS(AX[i]),&ai)==0)mn[i]=mx[i]=ai.value;
        else mn[i]=mx[i]=oc[i];
        last[i]=0;
    }
    int calibrating=1;
    int64_t cal_end=now_ms()+cal_ms;
    int ret=1;
    while(running){
        if(want_recal&&!calibrating){
            want_recal=0;calibrating=1;live_cal=0;live_state=2;cal_end=now_ms()+cal_ms;
            for(int i=0;i<4;i++){
                struct input_absinfo ai;
                if(ioctl(fd,EVIOCGABS(AX[i]),&ai)==0)mn[i]=mx[i]=ai.value;
                else mn[i]=mx[i]=oc[i];
            }
            printf("[CALIBRATION] CALIBRATION_IN_PROGRESS: kalibrasi ulang diminta.\n");
        }
        if(calibrating&&now_ms()>=cal_end){
            for(int i=0;i<4;i++){
                cen[i]=(mn[i]+mx[i])/2;noise[i]=(mx[i]-mn[i])/2;
                live_center[i]=cen[i];
                printf("[CALIBRATION] axis %d center=%d noise=%d\n",AX[i],cen[i],noise[i]);
            }
            have_cal=1;calibrating=0;live_cal=1;live_state=3;
            printf("[CALIBRATION] CALIBRATION_COMPLETE: Gamepad siap digunakan.\n");
            fflush(stdout);
            /* Pastikan semua sumbu mulai dari netral; tidak ada input kalibrasi yang diteruskan. */
            release_virtual_inputs();
        }
        struct pollfd p={fd,POLLIN,0};
        int r=poll(&p,1,calibrating?40:500);
        if(r<0&&errno!=EINTR)break;
        if(r<=0)continue;
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL))break;
        struct input_event ev[64],out_ev[128];
        ssize_t n=read(fd,ev,sizeof ev);
        if(n<0){if(errno==EAGAIN||errno==EINTR)continue;break;}
        size_t out_count=0;
        for(size_t k=0;k<(size_t)n/sizeof ev[0];k++){
            struct input_event e=ev[k];
            if(e.type==EV_ABS && (e.code==ABS_HAT0X||e.code==ABS_HAT0Y)){
                if(calibrating)continue;
                /* Forward both HAT axes and DPAD keys for broad game compatibility. */
                if(out_count<128)out_ev[out_count++]=e;
                int neg=e.code==ABS_HAT0X?BTN_DPAD_LEFT:BTN_DPAD_UP;
                int pos=e.code==ABS_HAT0X?BTN_DPAD_RIGHT:BTN_DPAD_DOWN;
                struct input_event ke;memset(&ke,0,sizeof ke);ke.type=EV_KEY;ke.code=neg;ke.value=e.value<0;
                if(out_count<128){out_ev[out_count++]=ke;pressed_key[neg]=(ke.value!=0);}
                ke.code=pos;ke.value=e.value>0;
                if(out_count<128){out_ev[out_count++]=ke;pressed_key[pos]=(ke.value!=0);}
            }else if(e.type==EV_ABS){
                int i=idx_of(e.code);
                if(i>=0){
                    live_raw[i]=e.value;
                    if(calibrating){
                        if(e.value<mn[i])mn[i]=e.value;
                        if(e.value>mx[i])mx[i]=e.value;
                        continue; /* input analog dibuang selama kalibrasi */
                    }
                    int dz=noise[i]+cfg_margin;if(dz<cfg_min_dz)dz=cfg_min_dz;
                    int center=cen[i]+cfg_offset[i];
                    if(center<lo[i])center=lo[i];if(center>hi[i])center=hi[i];
                    int off=fix(e.value,center,dz,lo[i],hi[i],half[i],cfg_kp);
                    int pct=sens[i*2+(off<0?0:1)];
                    long so=(long)off*100/(pct>0?pct:100);
                    if(so>half[i])so=half[i];if(so< -half[i])so=-half[i];
                    int ox=(i==0||i==2)?(int)so:stick_offset[i-1];
                    int oy=(i==1||i==3)?(int)so:stick_offset[i+1];
                    int ax=ox<0?-ox:ox,ay=oy<0?-oy:oy,dir;
                    if(ax*2<ay)dir=oy<0?0:1;else if(ay*2<ax)dir=ox<0?2:3;
                    else if(ox>=0&&oy<0)dir=4;else if(ox<0&&oy<0)dir=5;
                    else if(ox>=0&&oy>=0)dir=6;else dir=7;
                    int di=(i<2?0:8)+dir,radius=ax>ay?ax:ay;if(radius>half[i])radius=half[i];
                    int power=dir_min[di]+(int)((long)(dir_max[di]-dir_min[di])*radius/(half[i]>0?half[i]:1));
                    so=so*power/100;if(so>half[i])so=half[i];if(so< -half[i])so= -half[i];
                    int fixed_value=oc[i]+(int)so;
                    stick_offset[i]=(int)so;live_dz[i]=dz;
                    int vout=scale_to_virtual(fixed_value,lo[i],hi[i],-24000,24000);
                    vout=(int)((long)vout*stick_power[i/2]/100);
                    if(vout>24000)vout=24000;if(vout< -24000)vout=-24000;
                    live_fixed[i]=vout;
                    e.code=AX[i];
                    if(vout==last[i])continue;
                    last[i]=vout;e.value=vout;
                } else {
                    if(calibrating)continue;
                    if(e.code>ABS_MAX||!virtual_abs[e.code])continue;
                    struct input_absinfo ai;
                    if(ioctl(fd,EVIOCGABS(e.code),&ai)==0&&ai.maximum>ai.minimum){
                        int vmin=(e.code==ABS_HAT0X||e.code==ABS_HAT0Y||e.code==ABS_HAT1X||e.code==ABS_HAT1Y)?-1:
                            ((e.code==ABS_Z||e.code==ABS_RZ||e.code==ABS_GAS||e.code==ABS_BRAKE||e.code==ABS_THROTTLE||e.code==ABS_RUDDER)?0:-32768);
                        int vmax=vmin==-1?1:((vmin==0)?255:32767);
                        e.value=scale_to_virtual(e.value,ai.minimum,ai.maximum,vmin,vmax);
                    }
                }
                if(e.code<=ABS_MAX&&virtual_abs[e.code]&&out_count<128)out_ev[out_count++]=e;
            }else if(e.type==EV_KEY){
                if(calibrating)continue;
                last_key=e.code;
                /* BTN_Z (309) is the Capture key on this controller. */
                if(e.code==BTN_Z||e.code==KEY_RECORD||e.code==KEY_SYSRQ||e.code==KEY_CAMERA||e.code==BTN_TRIGGER_HAPPY1||e.code==BTN_TRIGGER_HAPPY2){capture_button_event(e.value);continue;}
                if(e.code<=KEY_MAX&&virtual_key[e.code]){
                    int out_code=e.code;
                    /* Compensate Android's standard mapping for Nintendo X/Y layout. */
                    if(out_code==BTN_NORTH)out_code=BTN_WEST;
                    else if(out_code==BTN_WEST)out_code=BTN_NORTH;
                    pressed_key[out_code]=(e.value!=0);
                    e.code=out_code;
                    if(out_count<128)out_ev[out_count++]=e;
                }
            }else if(e.type==EV_SYN){
                if(!calibrating&&out_count<128)out_ev[out_count++]=e;
            }else if(!calibrating&&e.type==EV_MSC){
                /* Metadata fisik bukan bagian dari virtual device; tidak diteruskan. */
            }
        }
        if(!calibrating)emit_batch(out_ev,out_count);
    }
    if(!running)ret=0;
    else printf("[PHYSICAL] PHYSICAL_GAMEPAD_DISCONNECTED: sumber input terputus; virtual gamepad tetap aktif.\n");
    release_virtual_inputs();
    live_conn=0;live_grab=0;live_cal=0;live_state=4;
    ioctl(fd,EVIOCGRAB,0);close(fd);
    return ret;
}

// ---------------- web server (hanya 127.0.0.1, wajib token) ----------------
static void reply(int c, const char *st, const char *type, const char *body, size_t len) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", st, type, len);
    (void)!write(c, h, (size_t)n);
    while (len) { ssize_t w = write(c, body, len); if (w <= 0) break; body += w; len -= (size_t)w; }
}
static void jreply(int c, const char *st, const char *b) { reply(c, st, "application/json", b, strlen(b)); }

static int hdr(const char *r, const char *name, char *out, int n) {
    size_t l = strlen(name); const char *p = r;
    while ((p = strchr(p, '\n'))) {
        p++;
        if (!strncasecmp(p, name, l)) {
            p += l; while (*p == ' ') p++;
            int i = 0; while (*p && *p != '\r' && *p != '\n' && i < n - 1) out[i++] = *p++;
            out[i] = 0; return 1;
        }
    }
    return 0;
}
static int read_req(int c, char *buf, int cap) {
    int n = 0, need = -1;
    while (n < cap - 1) {
        struct pollfd p = {c, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) break;
        int r = (int)read(c, buf + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += r; buf[n] = 0;
        char *h = strstr(buf, "\r\n\r\n");
        if (h) {
            if (need < 0) { char cl[16] = "0"; hdr(buf, "Content-Length:", cl, sizeof cl); need = (int)(h + 4 - buf) + atoi(cl); }
            if (n >= need) break;
        }
    }
    buf[n] = 0; return n;
}
static int json_int(const char *b, const char *key, int *v) {
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(b, pat); if (!p) return 0;
    p = strchr(p, ':'); if (!p) return 0;
    return sscanf(p + 1, " %d", v) == 1;
}
static int json_arr(const char *b, const char *key, int *v, int n) {
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(b, pat); if (!p || !(p = strchr(p, '['))) return 0;
    for (int i = 0; i < n; i++) {
        int x, used;
        if (sscanf(p + 1, " %d%n", &x, &used) != 1) return 0;
        v[i] = x; p += used; if (i < n - 1) { p = strchr(p, ','); if (!p) return 0; }
    }
    return 1;
}

static int take_shot(void) {
    mkdir("/sdcard/Pictures",0775); mkdir("/sdcard/Pictures/Padfix",0775);
    unlink(SHOT);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (!pid) {
        int nl = open("/dev/null", O_RDWR); dup2(nl, 1); dup2(nl, 2);
        execl("/system/bin/screencap", "screencap", "-p", SHOT, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 160; i++) {
        int st; pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
        usleep(50000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0); return -1;
}
static int toggle_screen_recording(void) {
    if(recording && recorder_pid>0) {
        kill(recorder_pid,SIGINT);
        int exited=0;
        for(int i=0;i<40;i++){int st;pid_t w=waitpid(recorder_pid,&st,WNOHANG);if(w==recorder_pid||(w<0&&errno==ECHILD)){exited=1;break;}usleep(100000);}
        if(!exited){kill(recorder_pid,SIGKILL);waitpid(recorder_pid,NULL,0);}
        recorder_pid=-1;recording=0;snprintf(capture_message,sizeof capture_message,"Rekaman dihentikan");
        printf("[CAPTURE] SCREEN_RECORD_STOPPED: %s\n",recording_path);return 0;
    }
    mkdir("/sdcard/Movies",0775);mkdir(RECORD_DIR,0775);
    time_t now=time(NULL);struct tm tmv;localtime_r(&now,&tmv);
    char stamp[40];strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",&tmv);
    snprintf(recording_path,sizeof recording_path,RECORD_DIR "/Padfix-%s.mp4",stamp);
    pid_t pid=fork();if(pid<0)return -1;
    if(pid==0){int nl=open("/dev/null",O_RDWR);if(nl>=0){dup2(nl,1);dup2(nl,2);if(nl>2)close(nl);}
        execl("/system/bin/screenrecord","screenrecord",recording_path,(char *)NULL);_exit(127);}
    recorder_pid=pid;recording=1;snprintf(capture_message,sizeof capture_message,"Rekaman dimulai");printf("[CAPTURE] SCREEN_RECORD_STARTED: %s\n",recording_path);return 0;
}
static void capture_button_event(int value) {
    int64_t now=now_ms();
    if(value && !capture_down){capture_down=1;capture_press_ms=now;capture_long_fired=0;
        if(capture_last_tap_ms>0&&now-capture_last_tap_ms<=600){capture_last_tap_ms=0;capture_long_fired=1;
            if(take_shot()==0){snprintf(capture_message,sizeof capture_message,"Screenshot tersimpan");printf("[CAPTURE] SCREENSHOT_SAVED: %s\n",SHOT);}else{snprintf(capture_message,sizeof capture_message,"Screenshot gagal");fprintf(stderr,"[CAPTURE] screenshot gagal.\n");}
        }else capture_last_tap_ms=now;
    }else if(!value&&capture_down){capture_down=0;if(capture_long_fired)capture_last_tap_ms=0;}
}
static void capture_button_tick(void) {
    if(recording&&recorder_pid>0){
        int st;pid_t w=waitpid(recorder_pid,&st,WNOHANG);
        if(w==recorder_pid||(w<0&&errno==ECHILD)){
            recorder_pid=-1;recording=0;
            if(w==recorder_pid&&WIFEXITED(st)&&WEXITSTATUS(st)==0)snprintf(capture_message,sizeof capture_message,"Rekaman selesai");
            else snprintf(capture_message,sizeof capture_message,"Rekaman gagal / terhenti");
            printf("[CAPTURE] SCREEN_RECORD_FINISHED: %s\n",recording_path);
        }
    }
    if(capture_down&&!capture_long_fired&&now_ms()-capture_press_ms>=2500){capture_long_fired=1;capture_last_tap_ms=0;
        if(toggle_screen_recording()<0)fprintf(stderr,"[CAPTURE] gagal mengendalikan screenrecord.\n");}
}
static char *slurp(const char *path, size_t *len, size_t max) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)sz; return b;
}

static const char *state_label(void) {
    switch(live_state){
        case 1:return "PHYSICAL_GAMEPAD_CONNECTED";
        case 2:return "CALIBRATION_IN_PROGRESS";
        case 3:return "CALIBRATION_COMPLETE";
        case 4:return "PHYSICAL_GAMEPAD_DISCONNECTED";
        default:return "WAITING_FOR_PHYSICAL_GAMEPAD";
    }
}
static const char *physical_label(void) {
    if(live_conn)return "PHYSICAL_GAMEPAD_CONNECTED";
    return live_state==4?"PHYSICAL_GAMEPAD_DISCONNECTED":"WAITING_FOR_PHYSICAL_GAMEPAD";
}
static const char *calibration_label(void) {
    if(live_state==2)return "CALIBRATION_IN_PROGRESS";
    if(live_state==3)return "CALIBRATION_COMPLETE";
    return "IDLE";
}

static void send_status(int c) {
    static char b[6144]; int o = 0;
    #define A(...) o += snprintf(b + o, sizeof b - (size_t)o, __VA_ARGS__)
    A("{\"running\":%s,\"connected\":%s,\"grabbed\":%s,\"calibrated\":%s,\"virtualActive\":%s,\"recording\":%s,\"recordingFile\":\"%s\",\"captureMessage\":\"%s\",\"power\":[%d,%d],\"axisMap\":[%d,%d,%d,%d],\"state\":\"%s\",\"virtualStatus\":\"%s\",\"physicalStatus\":\"%s\",\"calibrationStatus\":\"%s\",\"name\":\"%s\",\"margin\":%d,\"min_dz\":%d,\"kp\":%d,\"last_key\":%d,\"sens\":[",
      running ? "true" : "false", live_conn ? "true" : "false", live_grab ? "true" : "false", live_cal ? "true" : "false", virtual_active ? "true" : "false", recording ? "true" : "false", recording_path, capture_message, stick_power[0], stick_power[1], SRC_AX[0], SRC_AX[1], SRC_AX[2], SRC_AX[3], state_label(), virtual_active ? "VIRTUAL_DEVICE_ACTIVE" : "INACTIVE", physical_label(), calibration_label(), dev_name, cfg_margin, cfg_min_dz, cfg_kp, last_key);
    for (int i = 0; i < 8; i++) A("%s%d", i ? "," : "", sens[i]);
    A("],\"offset\":[%d,%d,%d,%d],\"raw\":[%d,%d,%d,%d],\"fixed\":[%d,%d,%d,%d],\"center\":[%d,%d,%d,%d],\"deadzone\":[%d,%d,%d,%d],\"buttons\":[",
      cfg_offset[0], cfg_offset[1], cfg_offset[2], cfg_offset[3], live_raw[0], live_raw[1], live_raw[2], live_raw[3], live_fixed[0], live_fixed[1], live_fixed[2], live_fixed[3],
      live_center[0], live_center[1], live_center[2], live_center[3], live_dz[0], live_dz[1], live_dz[2], live_dz[3]);
    int first = 1;
    for (int k = 1; k <= KEY_MAX; k++) if (have_key[k]) { A("%s%d", first ? "" : ",", k); first = 0; }
    A("],\"pressed\":["); first = 1;
    for (int k = 1; k <= KEY_MAX; k++) if (pressed_key[k]) { A("%s%d", first ? "" : ",", k); first = 0; }
    A("]}");
    #undef A
    jreply(c, "200 OK", b);
}

static void handle(int c) {
    char req[8192];
    if (read_req(c, req, sizeof req) <= 0) return;
    char m[8], path[128];
    if (sscanf(req, "%7s %127s", m, path) != 2) return;
    char *q = strchr(path, '?'); const char *qs = q ? q + 1 : ""; if (q) *q = 0;
    int get = !strcmp(m, "GET"), post = !strcmp(m, "POST");
    if (get && !strcmp(path, "/")) {
        if (strncmp(qs, "t=", 2) || strncmp(qs + 2, token, 16)) { jreply(c, "403 Forbidden", "{\"error\":\"buka URL lengkap dengan ?t=TOKEN dari log\"}"); return; }
        size_t len; char *pg = slurp(www, &len, 1 << 20);
        if (pg) { reply(c, "200 OK", "text/html; charset=utf-8", pg, len); free(pg); }
        else jreply(c, "404 Not Found", "{\"error\":\"index.html tidak ditemukan, jalankan padfix.sh\"}");
        return;
    }
    char t[64] = "";
    if (!hdr(req, "X-Token:", t, sizeof t) || strcmp(t, token)) { jreply(c, "403 Forbidden", "{\"error\":\"token salah\"}"); return; }
    const char *body = strstr(req, "\r\n\r\n"); body = body ? body + 4 : "";
    int v;
    if (get && !strcmp(path, "/api/status")) send_status(c);
    else if (get && !strcmp(path, "/api/shot.png")) {
        size_t len; char *img = slurp(SHOT, &len, 16u << 20);
        if (img) { reply(c, "200 OK", "image/png", img, len); free(img); } else jreply(c, "404 Not Found", "{\"error\":\"belum ada screenshot\"}");
    } else if (post && !strcmp(path, "/api/config")) {
        int a[8], off[4];
        if (json_int(body, "margin", &v) && v >= 0 && v <= 30000) cfg_margin = v;
        if (json_int(body, "min_dz", &v) && v >= 0 && v <= 30000) cfg_min_dz = v;
        if (json_int(body, "kp", &v) && v >= 0 && v <= 100) cfg_kp = v;
        if (json_arr(body, "sens", a, 8)) { int ok = 1; for (int i = 0; i < 8; i++) if (a[i] < 50 || a[i] > 200) ok = 0; if (ok) for (int i = 0; i < 8; i++) sens[i] = a[i]; }
        if (json_arr(body, "sensL", a, 4)) { int ok = 1; for (int i = 0; i < 4; i++) if (a[i] < 50 || a[i] > 200) ok = 0; if (ok) for (int i = 0; i < 4; i++) sens[i] = a[i]; }
        if (json_arr(body, "sensR", a, 4)) { int ok = 1; for (int i = 0; i < 4; i++) if (a[i] < 50 || a[i] > 200) ok = 0; if (ok) for (int i = 0; i < 4; i++) sens[i + 4] = a[i]; }
        if(json_int(body,"powerL",&v)&&v>=50&&v<=200)stick_power[0]=v;
        if(json_int(body,"powerR",&v)&&v>=50&&v<=200)stick_power[1]=v;
        if (json_arr(body, "offset", off, 4)) { int ok = 1; for (int i = 0; i < 4; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 4; i++) cfg_offset[i] = off[i]; }
        if (json_arr(body, "offsetL", off, 2)) { int ok = 1; for (int i = 0; i < 2; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 2; i++) cfg_offset[i] = off[i]; }
        if (json_arr(body, "offsetR", off, 2)) { int ok = 1; for (int i = 0; i < 2; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 2; i++) cfg_offset[i + 2] = off[i]; }
        int dm[16],dx[16];
        if(json_arr(body,"dirMin",dm,16)){int ok=1;for(int i=0;i<16;i++)if(dm[i]<50||dm[i]>200)ok=0;if(ok)for(int i=0;i<16;i++)dir_min[i]=dm[i];}
        if(json_arr(body,"dirMax",dx,16)){int ok=1;for(int i=0;i<16;i++)if(dx[i]<50||dx[i]>200)ok=0;if(ok)for(int i=0;i<16;i++)dir_max[i]=dx[i];}
        save_config();
        jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/reset")) {
        cfg_margin = 600; cfg_min_dz = 1500; cfg_kp = 0;
        for (int i = 0; i < 8; i++) sens[i] = 100;
        stick_power[0]=stick_power[1]=100;
        for (int i = 0; i < 4; i++) cfg_offset[i] = 0;
        for (int i = 0; i < 16; i++) dir_min[i] = dir_max[i] = 100;
        save_config();
        jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/recal")) { want_recal = 1; jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/screenshot")) {
        if (take_shot() == 0) {snprintf(capture_message,sizeof capture_message,"Screenshot tersimpan");jreply(c, "200 OK", "{\"ok\":true}");}
        else {snprintf(capture_message,sizeof capture_message,"Screenshot gagal");jreply(c, "500 Internal Server Error", "{\"error\":\"screencap gagal; periksa izin Shizuku/shell\"}");}
    } else if (post && !strcmp(path, "/api/record")) {
        if(toggle_screen_recording()==0)jreply(c,"200 OK","{\"ok\":true}");
        else jreply(c,"500 Internal Server Error","{\"error\":\"screenrecord gagal dijalankan\"}");
    } else if (post && !strcmp(path, "/api/stop")) { jreply(c, "200 OK", "{\"ok\":true}"); running = 0;
    } else jreply(c, "404 Not Found", "{\"error\":\"not found\"}");
}

static void *web_main(void *unused) {
    (void)unused;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return NULL;
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(PORT);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) {
        fprintf(stderr, "Web server gagal bind 127.0.0.1:%d (%s). Mungkin padfix lama masih jalan.\n", PORT, strerror(errno));
        close(s); return NULL;
    }
    while (running) {
        struct pollfd p = {s, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;
        int c = accept(s, NULL, NULL); if (c < 0) continue;
        handle(c); close(c);
    }
    close(s); return NULL;
}

int main(int argc, char **argv) {
    char fixed[64] = {0};
    int cal_ms = 3000, opt, demo = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "e:n:c:m:z:k:w:d")) != -1) {
        if (opt == 'e') snprintf(fixed, sizeof fixed, "%s", optarg);
        else if (opt == 'n') match = optarg;
        else if (opt == 'c') cal_ms = atoi(optarg);
        else if (opt == 'm') cfg_margin = atoi(optarg);
        else if (opt == 'z') cfg_min_dz = atoi(optarg);
        else if (opt == 'k') cfg_kp = atoi(optarg);
        else if (opt == 'w') www = optarg;
        else if (opt == 'd') demo = 1;
    }
    load_config();
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    unsigned char rnd[8] = {0};
    int rf = open("/dev/urandom", O_RDONLY);
    if (rf < 0 || read(rf, rnd, 8) != 8) { fprintf(stderr, "Tidak bisa membuat token acak.\n"); return 1; }
    close(rf);
    for (int i = 0; i < 8; i++) snprintf(token + i * 2, 3, "%02x", rnd[i]);
    pthread_t th;
    if (pthread_create(&th, NULL, web_main, NULL) != 0) { fprintf(stderr, "Gagal membuat thread web.\n"); return 1; }
    printf("Dashboard: http://127.0.0.1:%d/?t=%s\n", PORT, token);
    fflush(stdout); // MainActivity reads this line from the startup log to open the tokenized URL.
    if (demo) { while (running) sleep(1); pthread_join(th, NULL); return 0; }
    if (!demo && create_virtual_device() < 0) {
        fprintf(stderr, "[VIRTUAL] gagal membuat virtual gamepad persisten; backend dihentikan.\n");
        running=0; pthread_join(th,NULL); return 1;
    }
    live_state=0;
    pthread_t keepalive_thread;
    int keepalive_started=pthread_create(&keepalive_thread,NULL,r3_keepalive_main,NULL)==0;
    if(!keepalive_started)fprintf(stderr,"[VIRTUAL] gagal menjalankan thread R3 keep-alive.\\n");
    int announced = 0;
    while (running) {
        int r = session(fixed, cal_ms, &announced);
        if (r < 0) { running = 0; break; }
        if (r == 1 && running) sleep(1);
    }
    if(keepalive_started)pthread_join(keepalive_thread,NULL);
    if (virtual_active) {
        release_virtual_inputs();
        pthread_mutex_lock(&um);
        int vu=ufd; ufd=-1;
        pthread_mutex_unlock(&um);
        if(vu>=0){ioctl(vu,UI_DEV_DESTROY);close(vu);}
        virtual_active=0;
        printf("[VIRTUAL] virtual gamepad dihancurkan hanya karena Padfix dihentikan.\\n");
    }
    pthread_join(th, NULL);
    printf("Berhenti.\n");
    return 0;
}