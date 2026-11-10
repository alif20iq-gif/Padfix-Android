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
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <math.h>

#define BPL (sizeof(unsigned long) * 8)
#define NB(x) ((((x) - 1) / BPL) + 1)
#define PORT 8765
#define SHOT "/data/local/tmp/padfix_shot.png"

static volatile sig_atomic_t running = 1;
static volatile int cfg_margin = 600, cfg_min_dz = 1500, cfg_kp = 0;
// sens[axis*2+0/-1] = faktor jangkauan per arah; 50% memberi power 2x, 100% normal, 200% memberi power 0.5x.
static volatile int sens[8] = {100,100,100,100,100,100,100,100};
// Offset manual per axis: koreksi titik tengah hasil kalibrasi.
static volatile int cfg_offset[4] = {0,0,0,0};
static volatile int circular_enabled[2] = {0,0}, circular_target[2] = {20000,20000};
static volatile int circular_center[4] = {-16379,16507,-16379,16507};
static volatile int circular_points[16] = {0,-9842,9842,0,0,22574,-22350,0,0,-9842,9842,0,0,22574,-22350,0};
static volatile int live_raw[4], live_fixed[4], live_center[4], live_dz[4];
static volatile int live_cal = 0, live_conn = 0, live_grab = 0, want_recal = 0, last_key = 0;
static unsigned char have_key[KEY_MAX + 1];
static char dev_name[128] = "";
static int ufd = -1;
static pthread_mutex_t um = PTHREAD_MUTEX_INITIALIZER;
static char token[17];
static const char *www = "/data/local/tmp/padfix_index.html";
static const char *match = "Pro Controller";
static const int AX[4] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
static unsigned char pressed_key[KEY_MAX + 1];

static const char *config_path = "/data/local/tmp/padfix.conf";
static void save_config(void) {
    char tmp[96]; snprintf(tmp,sizeof tmp,"%s.tmp",config_path);
    FILE *f=fopen(tmp,"w"); if(!f)return;
    fprintf(f,"%d %d %d\n",cfg_margin,cfg_min_dz,cfg_kp);
    for(int i=0;i<8;i++)fprintf(f,"%d%c",sens[i],i==7?'\n':' ');
    for(int i=0;i<4;i++)fprintf(f,"%d%c",cfg_offset[i],i==3?'\n':' ');
    for(int i=0;i<2;i++)fprintf(f,"%d %d ",circular_enabled[i],circular_target[i]);
    fputc('\n',f);
    for(int i=0;i<4;i++)fprintf(f,"%d%c",circular_center[i],i==3?'\n':' ');
    for(int i=0;i<16;i++)fprintf(f,"%d%c",circular_points[i],i==15?'\n':' ');
    if(fclose(f)==0){if(rename(tmp,config_path)!=0)unlink(tmp);}else unlink(tmp);
}
static void load_config(void) {
    FILE *f=fopen(config_path,"r");if(!f)return;
    int m,z,k,s[8],o[4],ce[2],ct[2],cc[4],cp[16],ok=fscanf(f,"%d%d%d",&m,&z,&k)==3;
    for(int i=0;ok&&i<8;i++)ok=fscanf(f,"%d",&s[i])==1&&s[i]>=50&&s[i]<=200;
    for(int i=0;ok&&i<4;i++)ok=fscanf(f,"%d",&o[i])==1&&o[i]>=-30000&&o[i]<=30000;
    int has_circular=0;
    if(ok && fscanf(f,"%d%d",&ce[0],&ct[0])==2 && fscanf(f,"%d%d",&ce[1],&ct[1])==2) {
        has_circular=1;
        for(int i=0;i<2;i++)if(ce[i]<0||ce[i]>1||ct[i]<1000||ct[i]>30000)ok=0;
        for(int i=0;ok&&i<4;i++)ok=fscanf(f,"%d",&cc[i])==1&&cc[i]>=-32768&&cc[i]<=32767;
        for(int i=0;ok&&i<16;i++)ok=fscanf(f,"%d",&cp[i])==1&&cp[i]>=-32768&&cp[i]<=32767;
    }
    fclose(f);
    if(ok&&m>=0&&m<=30000&&z>=0&&z<=30000&&k>=0&&k<=100){
        cfg_margin=m;cfg_min_dz=z;cfg_kp=k;
        for(int i=0;i<8;i++)sens[i]=s[i];
        for(int i=0;i<4;i++)cfg_offset[i]=o[i];
        if(has_circular){
            for(int i=0;i<2;i++){circular_enabled[i]=ce[i];circular_target[i]=ct[i];}
            for(int i=0;i<4;i++)circular_center[i]=cc[i];
            for(int i=0;i<16;i++)circular_points[i]=cp[i];
        }
        printf("Konfigurasi terakhir dipulihkan.\n");
    }
}


static void on_sig(int s) { (void)s; running = 0; }
static int idx_of(int code) { for (int i = 0; i < 4; i++) if (AX[i] == code) return i; return -1; }
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
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        int ok = ioctl(fd, EVIOCGNAME(sizeof name), name) > 0 && strstr(name, match) &&
                 !strstr(name, "IMU") && !strstr(name, "(padfix)");   // jangan ambil device virtual sendiri
        close(fd);
        if (ok) { snprintf(out, n, "%s", p); return 0; }
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

// ---------------- kalibrasi yang bertahan antar reconnect ----------------
static int have_cal = 0, cen[4], noise[4];

static void circular_map(int stick,int x,int y,int *ox,int *oy) {
    int base=stick*8;
    double a=atan2((double)y,(double)x); if(a<0)a+=2.0*M_PI;
    const int order[4]={1,2,3,0};
    int sector=(int)(a/(M_PI/2.0)); if(sector>3)sector=3;
    double frac=(a-sector*(M_PI/2.0))/(M_PI/2.0);
    int p1=order[sector],p2=order[(sector+1)%4];
    double r1=hypot((double)circular_points[base+p1*2],(double)circular_points[base+p1*2+1]);
    double r2=hypot((double)circular_points[base+p2*2],(double)circular_points[base+p2*2+1]);
    double boundary=r1+(r2-r1)*frac; if(boundary<1.0)boundary=1.0;
    double scale=(double)circular_target[stick]/boundary;
    double vx=x*scale,vy=y*scale,r=hypot(vx,vy),target=circular_target[stick];
    if(r>target){vx*=target/r;vy*=target/r;}
    *ox=(int)lrint(vx);*oy=(int)lrint(vy);
}
 
// Satu sesi = satu koneksi gamepad. return 1: coba lagi, 0: selesai, -1: fatal.
static int session(const char *fixed, int cal_ms, int *announced) {
    char path[64] = {0};
    if (fixed[0]) snprintf(path, sizeof path, "%s", fixed);
    else if (find_pad(path, sizeof path) < 0) {
        if (!*announced) { printf("Menunggu gamepad...\n"); *announced = 1; }
        return 1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 1;
    unsigned long keybits[NB(KEY_MAX + 1)], absbits[NB(ABS_MAX + 1)];
    memset(keybits, 0, sizeof keybits); memset(absbits, 0, sizeof absbits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybits), keybits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits);
    struct input_id id; ioctl(fd, EVIOCGID, &id);
    char rname[128] = {0}; ioctl(fd, EVIOCGNAME(sizeof rname), rname);

    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (u < 0) { perror("open /dev/uinput (butuh user shell/root)"); close(fd); return -1; }
    ioctl(u, UI_SET_EVBIT, EV_KEY); ioctl(u, UI_SET_EVBIT, EV_ABS); ioctl(u, UI_SET_EVBIT, EV_SYN);
    memset(have_key, 0, sizeof have_key); memset(pressed_key, 0, sizeof pressed_key);
    for (int k = 0; k <= KEY_MAX; k++) if (tbit(k, keybits)) { ioctl(u, UI_SET_KEYBIT, k); have_key[k] = 1; }
    // Beberapa game Android hanya mengenali D-pad sebagai EV_KEY, bukan ABS_HAT.
    const int dpad_keys[] = {BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT};
    for (size_t q = 0; q < sizeof dpad_keys / sizeof dpad_keys[0]; q++) {
        ioctl(u, UI_SET_KEYBIT, dpad_keys[q]);
        have_key[dpad_keys[q]] = 1;
    }

    struct uinput_user_dev ud; memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "%.60s (padfix)", rname);
    ud.id = id;
    int lo[4] = {0}, hi[4] = {0}, half[4] = {0}, oc[4] = {0};
    for (int a = 0; a <= ABS_MAX; a++) {
        if (!tbit(a, absbits)) continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(a), &ai) < 0) continue;
        ioctl(u, UI_SET_ABSBIT, a);
        ud.absmin[a] = ai.minimum; ud.absmax[a] = ai.maximum;
        int i = idx_of(a);
        if (i >= 0) { lo[i] = ai.minimum; hi[i] = ai.maximum; half[i] = (hi[i] - lo[i]) / 2; oc[i] = (hi[i] + lo[i]) / 2; }
        else { ud.absfuzz[a] = ai.fuzz; ud.absflat[a] = ai.flat; }
    }
    if (write(u, &ud, sizeof ud) != (ssize_t)sizeof ud || ioctl(u, UI_DEV_CREATE) < 0) {
        perror("uinput create"); close(u); close(fd); return -1;
    }
    live_grab = (ioctl(fd, EVIOCGRAB, 1) == 0);
    if (!live_grab) perror("EVIOCGRAB (input fisik mungkin masih masuk ke browser)");
    pthread_mutex_lock(&um); ufd = u; pthread_mutex_unlock(&um);
    snprintf(dev_name, sizeof dev_name, "%s", rname);
    for (char *p = dev_name; *p; p++) if (*p == '"' || *p == '\\') *p = '_';
    live_conn = 1; *announced = 0;
    printf("Gamepad tersambung: %s (%s)\n", rname, path); fflush(stdout);

    int calibrating = !have_cal || want_recal, mn[4], mx[4], last[4];
    int64_t cal_end = 0;
    if (calibrating) {
        want_recal = 0; live_cal = 0; cal_end = now_ms() + cal_ms;
        for (int i = 0; i < 4; i++) { struct input_absinfo ai; ioctl(fd, EVIOCGABS(AX[i]), &ai); mn[i] = mx[i] = ai.value; }
        printf("Kalibrasi %d ms: LEPAS kedua stik...\n", cal_ms); fflush(stdout);
    } else live_cal = 1;
    for (int i = 0; i < 4; i++) last[i] = oc[i];

    int ret = 1;
    while (running) {
        if (want_recal && !calibrating) {   // diminta dari dashboard
            want_recal = 0; calibrating = 1; live_cal = 0; cal_end = now_ms() + cal_ms;
            for (int i = 0; i < 4; i++) { struct input_absinfo ai; ioctl(fd, EVIOCGABS(AX[i]), &ai); mn[i] = mx[i] = ai.value; }
        }
        if (calibrating && now_ms() >= cal_end) {
            for (int i = 0; i < 4; i++) {
                cen[i] = (mn[i] + mx[i]) / 2; noise[i] = (mx[i] - mn[i]) / 2;
                live_center[i] = cen[i];
                printf("axis %d: min=%d max=%d -> center=%d noise=%d\n", AX[i], mn[i], mx[i], cen[i], noise[i]);
            }
            have_cal = 1; calibrating = 0; live_cal = 1; fflush(stdout);
        }
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, calibrating ? 100 : 500);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        struct input_event ev[64], out_ev[64];
        ssize_t n = read(fd, ev, sizeof ev);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) continue; break; }   // ENODEV = dilepas
        size_t out_count = 0;
        for (size_t k = 0; k < (size_t)n / sizeof ev[0]; k++) {
            struct input_event e = ev[k];
            if (e.type == EV_ABS && (e.code == ABS_HAT0X || e.code == ABS_HAT0Y)) {
                // Terjemahkan HAT menjadi tombol D-pad virtual. Nilai 0 melepas
                // kedua arah pada sumbu tersebut; -1/+1 menekan arah yang sesuai.
                int negative_key = (e.code == ABS_HAT0X) ? BTN_DPAD_LEFT : BTN_DPAD_UP;
                int positive_key = (e.code == ABS_HAT0X) ? BTN_DPAD_RIGHT : BTN_DPAD_DOWN;
                struct input_event k;
                memset(&k, 0, sizeof k);
                k.type = EV_KEY; k.code = negative_key; k.value = (e.value < 0) ? 1 : 0;
                if (out_count < sizeof out_ev / sizeof out_ev[0]) out_ev[out_count++] = k;
                k.code = positive_key; k.value = (e.value > 0) ? 1 : 0;
                if (out_count < sizeof out_ev / sizeof out_ev[0]) out_ev[out_count++] = k;
            } else if (e.type == EV_ABS) {
                int i = idx_of(e.code);
                if (i >= 0) {
                    live_raw[i] = e.value;
                    int out;
                    if (calibrating) {
                        if (e.value < mn[i]) mn[i] = e.value;
                        if (e.value > mx[i]) mx[i] = e.value;
                        out = oc[i];
                    } else {
                        int dz = noise[i] + cfg_margin; if (dz < cfg_min_dz) dz = cfg_min_dz;
                        int center = cen[i] + cfg_offset[i];
                        if (center < lo[i]) center = lo[i];
                        if (center > hi[i]) center = hi[i];
                        int stick=i/2, axis=i%2;
                        if(circular_enabled[stick]) {
                            int cx=circular_center[stick*2],cy=circular_center[stick*2+1];
                            int vx=live_raw[stick*2]-cx,vy=live_raw[stick*2+1]-cy,mx=0,my=0;
                            circular_map(stick,vx,vy,&mx,&my);
                            int mapped=axis?my:mx;
                            if(mapped>half[i])mapped=half[i];if(mapped< -half[i])mapped=-half[i];
                            out=oc[i]+mapped;live_fixed[i]=mapped;live_dz[i]=dz;
                            if(out==last[i])continue;
                            last[i]=out;e.value=out;
                            if(out_count<sizeof out_ev/sizeof out_ev[0])out_ev[out_count++]=e;
                            continue;
                        }
                        int off = fix(e.value, center, dz, lo[i], hi[i], half[i], cfg_kp);
                        int pct = sens[i * 2 + (off < 0 ? 0 : 1)];
                        /* Sensitivitas arah: nilai lebih kecil memperkuat power; 50%=2x, 100%=1x, 200%=0.5x. */
                        long so = (long)off * 100 / (pct > 0 ? pct : 100);
                        if (so > half[i]) so = half[i];
                        if (so < -half[i]) so = -half[i];
                        out = oc[i] + (int)so;
                        live_dz[i] = dz;
                    }
                    live_fixed[i] = out - oc[i];
                    if (out == last[i]) continue;
                    last[i] = out; e.value = out;
                }
                if (out_count < sizeof out_ev / sizeof out_ev[0]) out_ev[out_count++] = e;
            } else if (e.type == EV_KEY) {
                // PASS-THROUGH MURNI: tombol fisik tidak di-remap, difilter,
                // atau diubah nilainya. Padfix hanya memproses ABS_X/Y/RX/RY.
                last_key = e.code;
                if (e.code <= KEY_MAX) pressed_key[e.code] = (e.value != 0);
                if (out_count < sizeof out_ev / sizeof out_ev[0]) out_ev[out_count++] = e;
            } else {
                // Semua event selain empat sumbu analog dan HAT D-pad diteruskan apa adanya.
                if (out_count < sizeof out_ev / sizeof out_ev[0]) out_ev[out_count++] = e;
            }
        }
        emit_batch(out_ev, out_count);
    }
    if (!running) ret = 0; else { printf("Gamepad terlepas.\n"); fflush(stdout); }
    live_conn = 0; live_grab = 0;
    ioctl(fd, EVIOCGRAB, 0);
    pthread_mutex_lock(&um); ufd = -1; pthread_mutex_unlock(&um);
    ioctl(u, UI_DEV_DESTROY); close(u); close(fd);
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
static char *slurp(const char *path, size_t *len, size_t max) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > max) { fclose(f); return NULL; }
    char *b = malloc((size_t)sz); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)sz; return b;
}

static void send_status(int c) {
    static char b[6144]; int o = 0;
    #define A(...) o += snprintf(b + o, sizeof b - (size_t)o, __VA_ARGS__)
    A("{\"running\":%s,\"connected\":%s,\"grabbed\":%s,\"calibrated\":%s,\"name\":\"%s\",\"margin\":%d,\"min_dz\":%d,\"kp\":%d,\"last_key\":%d,\"sens\":[",
      running ? "true" : "false", live_conn ? "true" : "false", live_grab ? "true" : "false", live_cal ? "true" : "false", dev_name, cfg_margin, cfg_min_dz, cfg_kp, last_key);
    for (int i = 0; i < 8; i++) A("%s%d", i ? "," : "", sens[i]);
    A("],\"offset\":[%d,%d,%d,%d],\"raw\":[%d,%d,%d,%d],\"fixed\":[%d,%d,%d,%d],\"center\":[%d,%d,%d,%d],\"deadzone\":[%d,%d,%d,%d]",
      cfg_offset[0], cfg_offset[1], cfg_offset[2], cfg_offset[3], live_raw[0], live_raw[1], live_raw[2], live_raw[3], live_fixed[0], live_fixed[1], live_fixed[2], live_fixed[3],
      live_center[0], live_center[1], live_center[2], live_center[3], live_dz[0], live_dz[1], live_dz[2], live_dz[3]);
    A(",\"circular_enabled\":[%d,%d],\"circular_target\":[%d,%d],\"circular_center\":[%d,%d,%d,%d],\"circular_points\":[",
      circular_enabled[0],circular_enabled[1],circular_target[0],circular_target[1],circular_center[0],circular_center[1],circular_center[2],circular_center[3]);
    for(int i=0;i<16;i++)A("%s%d",i?",":"",circular_points[i]);
    A("],\"buttons\":[");
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
        if (json_arr(body, "offset", off, 4)) { int ok = 1; for (int i = 0; i < 4; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 4; i++) cfg_offset[i] = off[i]; }
        if (json_arr(body, "offsetL", off, 2)) { int ok = 1; for (int i = 0; i < 2; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 2; i++) cfg_offset[i] = off[i]; }
        if (json_arr(body, "offsetR", off, 2)) { int ok = 1; for (int i = 0; i < 2; i++) if (off[i] < -30000 || off[i] > 30000) ok = 0; if (ok) for (int i = 0; i < 2; i++) cfg_offset[i + 2] = off[i]; }
        int ce[2],ct[2],cc[4],cp[16];
        if(json_arr(body,"circular_enabled",ce,2))for(int i=0;i<2;i++)if(ce[i]>=0&&ce[i]<=1)circular_enabled[i]=ce[i];
        if(json_arr(body,"circular_target",ct,2))for(int i=0;i<2;i++)if(ct[i]>=1000&&ct[i]<=30000)circular_target[i]=ct[i];
        if(json_arr(body,"circular_center",cc,4)){int ok=1;for(int i=0;i<4;i++)if(cc[i]<-32768||cc[i]>32767)ok=0;if(ok)for(int i=0;i<4;i++)circular_center[i]=cc[i];}
        if(json_arr(body,"circular_points",cp,16)){int ok=1;for(int i=0;i<16;i++)if(cp[i]<-32768||cp[i]>32767)ok=0;if(ok)for(int i=0;i<16;i++)circular_points[i]=cp[i];}
        save_config();
        jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/reset")) {
        cfg_margin = 600; cfg_min_dz = 1500; cfg_kp = 0;
        for (int i = 0; i < 8; i++) sens[i] = 100;
        for (int i = 0; i < 4; i++) cfg_offset[i] = 0;
        for(int i=0;i<2;i++){circular_enabled[i]=0;circular_target[i]=20000;}
        for(int i=0;i<4;i++)circular_center[i]=(i%2==0?-16379:16507);
        const int defaults[16]={0,-9842,9842,0,0,22574,-22350,0,0,-9842,9842,0,0,22574,-22350,0};
        for(int i=0;i<16;i++)circular_points[i]=defaults[i];
        save_config();
        jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/recal")) { want_recal = 1; jreply(c, "200 OK", "{\"ok\":true}");
    } else if (post && !strcmp(path, "/api/screenshot")) {
        if (take_shot() == 0) jreply(c, "200 OK", "{\"ok\":true}");
        else jreply(c, "500 Internal Server Error", "{\"error\":\"screencap gagal (butuh user shell/ADB/Shizuku; app FLAG_SECURE tampil hitam)\"}");
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
    if (demo) { while (running) sleep(1); pthread_join(th, NULL); return 0; }
    int announced = 0;
    while (running) {
        int r = session(fixed, cal_ms, &announced);
        if (r < 0) { running = 0; break; }
        if (r == 1 && running) sleep(1);
    }
    pthread_join(th, NULL);
    printf("Berhenti.\n");
    return 0;
}