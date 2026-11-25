#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXC 128
#define MAXN 100000
#define LINE 512
typedef struct { int fd; char buf[LINE]; size_t used; int id; char role[16]; } Client;
typedef struct { int fd, index, id; } Waiter;
static volatile sig_atomic_t stopping;
static void on_signal(int s){(void)s;stopping=1;}
static int db[2][MAXN], n=5, active, version=1, stages=3, strategy=0, writing, writer=-1, writer_id, next_stage, write_version;
static int minv=1,maxv=100,ops=5,prep_ms=100,confirm_ms=100,read_ms=100;
static unsigned seed_state=42;
static long long reads,pubs,stages_done,aborted;
static Waiter waiters[128]; static int nwait;
static FILE *log_file;
static void log_event(const char *fmt,...){va_list a,b;va_start(a,fmt);va_copy(b,a);vprintf(fmt,a);puts("");if(log_file){vfprintf(log_file,fmt,b);fputc('\n',log_file);fflush(log_file);}va_end(b);va_end(a);}
static void answer(int fd,const char *fmt,...){char b[LINE];va_list a;va_start(a,fmt);vsnprintf(b,sizeof(b),fmt,a);va_end(a);size_t z=strlen(b);b[z++]='\n';(void)send(fd,b,z,MSG_NOSIGNAL);}
static unsigned rnd(void){seed_state=seed_state*1664525u+1013904223u;return seed_state;}
static int cmp_int(const void *a,const void *b){int x=*(const int*)a,y=*(const int*)b;return (x>y)-(x<y);}
static void read_result(int fd,int id,int idx){int v=db[active][idx],ver=version;reads++;answer(fd,"DATA %d %d %d %d %lld",ver,idx,v,idx+1,(long long)v*(idx+1));log_event("[Reader %d] версия=%d индекс=%d значение=%d запись=%d произведение=%lld",id,ver,idx,v,idx+1,(long long)v*(idx+1));}
static void flush_waiters(void){for(int i=0;i<nwait;i++)if(waiters[i].fd>=0)read_result(waiters[i].fd,waiters[i].id,waiters[i].index);nwait=0;}
static void abort_write(const char *why){if(writing){log_event("[Writer %d] %s; версия %d сохранена",writer_id,why,version);writing=0;writer=-1;aborted++;flush_waiters();}}
static void process(Client *c,char *s){
 int x;
 if(sscanf(s,"HELLO %15s %d",c->role,&x)==2){c->id=x;answer(c->fd,"WELCOME %d %d %d %d %d %d %d %d",version,n,strategy,stages,ops,read_ms,prep_ms,confirm_ms);log_event("[TCP] подключён клиент %s #%d",c->role,x);return;}
 if(sscanf(s,"READ %d",&x)==1){if(x<0||x>=n){answer(c->fd,"ERR index");return;}if(strategy==1&&writing){if(nwait==128){answer(c->fd,"ERR wait-queue-full");return;}waiters[nwait++]=(Waiter){c->fd,x,c->id};log_event("[Reader %d] ожидает публикации версии %d",c->id,version+1);}else read_result(c->fd,c->id,x);return;}
 unsigned int write_seed;
 if(sscanf(s,"BEGIN %u",&write_seed)==1){if(writing){answer(c->fd,"ERR busy");return;}writing=1;writer=c->fd;writer_id=c->id;write_version=version+1;next_stage=0;memcpy(db[1-active],db[active],(size_t)n*sizeof(int));seed_state=write_seed?write_seed:1;log_event("[Writer %d] начал подготовку версии %d",writer_id,write_version);answer(c->fd,"OK BEGIN %d",write_version);return;}
 if(sscanf(s,"STAGE %d",&x)==1){if(!writing||writer!=c->fd||x!=next_stage||x>=stages){answer(c->fd,"ERR stage");return;}int l=(int)((long long)x*n/stages),r=(int)((long long)(x+1)*n/stages);for(int i=l;i<r;i++)db[1-active][i]=minv+(int)(rnd()%(unsigned)(maxv-minv+1));next_stage++;stages_done++;log_event("[Writer %d] завершил стадию %d/%d",writer_id,x+1,stages);answer(c->fd,"OK STAGE %d",x);return;}
 if(!strcmp(s,"PUBLISH")){if(!writing||writer!=c->fd||next_stage!=stages){answer(c->fd,"ERR incomplete");return;}int *a=db[1-active];qsort(a,(size_t)n,sizeof(int),cmp_int);for(int i=0;i<n;i++)if(a[i]<=0){answer(c->fd,"ERR invalid");abort_write("проверка не пройдена");return;}active=1-active;version=write_version;pubs++;writing=0;writer=-1;log_event("[Writer %d] версия %d проверена и опубликована",writer_id,version);answer(c->fd,"OK PUBLISHED %d",version);flush_waiters();return;}
 if(!strcmp(s,"ABORT")){if(writing&&writer==c->fd){answer(c->fd,"OK ABORT");abort_write("подготовка отменена");}else answer(c->fd,"ERR no-write");return;}
 if(!strcmp(s,"STATUS")){answer(c->fd,"STATUS version=%d size=%d writing=%d writer=%d reads=%lld publications=%lld stages=%lld aborted=%lld strategy=%s",version,n,writing,writer_id,reads,pubs,stages_done,aborted,strategy?"WAIT_NEW":"READ_OLD");return;}
 if(!strcmp(s,"QUIT")){answer(c->fd,"BYE");return;} answer(c->fd,"ERR command");
}
static int server_main(int argc,char **argv){
 const char *ip="0.0.0.0",*port="5050";for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--bind")&&i+1<argc)ip=argv[++i];else if(!strcmp(argv[i],"--port")&&i+1<argc)port=argv[++i];else if(!strcmp(argv[i],"--strategy")&&i+1<argc){char *v=argv[++i];if(!strcmp(v,"WAIT_NEW"))strategy=1;else if(strcmp(v,"READ_OLD"))return 2;}else if(!strcmp(argv[i],"--ops")&&i+1<argc)ops=atoi(argv[++i]);else if(!strcmp(argv[i],"--stages")&&i+1<argc)stages=atoi(argv[++i]);else if(!strcmp(argv[i],"--size")&&i+1<argc)n=atoi(argv[++i]);else if(!strcmp(argv[i],"--min")&&i+1<argc)minv=atoi(argv[++i]);else if(!strcmp(argv[i],"--max")&&i+1<argc)maxv=atoi(argv[++i]);else if(!strcmp(argv[i],"--seed")&&i+1<argc)seed_state=(unsigned)atoi(argv[++i]);else if(!strcmp(argv[i],"--read-ms")&&i+1<argc)read_ms=atoi(argv[++i]);else if(!strcmp(argv[i],"--prep-ms")&&i+1<argc)prep_ms=atoi(argv[++i]);else if(!strcmp(argv[i],"--confirm-ms")&&i+1<argc)confirm_ms=atoi(argv[++i]);else{fprintf(stderr,"server [--bind IP] [--port PORT] [--strategy READ_OLD|WAIT_NEW] [--ops N] [--stages N] [--size N]\n");return 2;}}
 if(n<1||n>MAXN||stages<1||stages>n||ops<0||minv<1||maxv<minv||read_ms<0||prep_ms<0||confirm_ms<0){fprintf(stderr,"Invalid server parameters\n");return 2;}int initial[]={2,5,8,12,19};for(int i=0;i<n;i++)db[0][i]=db[1][i]=i<5?initial[i]:20+i;int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){perror("socket");return 1;}int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons((unsigned short)atoi(port));if(inet_pton(AF_INET,ip,&a.sin_addr)!=1||bind(fd,(struct sockaddr*)&a,sizeof(a))<0||listen(fd,32)<0){perror("bind/listen");close(fd);return 1;}signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);mkdir("logs",0755);log_file=fopen("logs/idz4_server.log","a");log_event("[SERVER] TCP %s:%s; size=%d strategy=%s",ip,port,n,strategy?"WAIT_NEW":"READ_OLD");
 Client c[MAXC];for(int i=0;i<MAXC;i++)c[i].fd=-1;
 while(!stopping){struct pollfd p[MAXC+1];int map[MAXC+1],cnt=1;p[0]=(struct pollfd){fd,POLLIN,0};for(int i=0;i<MAXC;i++)if(c[i].fd>=0){p[cnt]=(struct pollfd){c[i].fd,POLLIN,0};map[cnt++]=i;}int z=poll(p,cnt,500);if(z<0){if(errno==EINTR)continue;break;}if(p[0].revents&POLLIN){int q=accept(fd,NULL,NULL),k=-1;if(q>=0){for(int i=0;i<MAXC;i++)if(c[i].fd<0){k=i;break;}if(k<0)close(q);else{c[k].fd=q;c[k].used=0;c[k].id=0;}}}
 for(int j=1;j<cnt;j++)if(p[j].revents&(POLLIN|POLLHUP|POLLERR)){int k=map[j];char b[256];ssize_t r=recv(c[k].fd,b,sizeof(b),0);if(r<=0){if(writer==c[k].fd)abort_write("соединение писателя потеряно");for(int w=0;w<nwait;w++)if(waiters[w].fd==c[k].fd)waiters[w].fd=-1;close(c[k].fd);c[k].fd=-1;continue;}for(ssize_t t=0;t<r;t++){char ch=b[t];if(ch=='\n'){c[k].buf[c[k].used]=0;process(&c[k],c[k].buf);c[k].used=0;}else if(ch!='\r'&&c[k].used+1<LINE)c[k].buf[c[k].used++]=ch;else{close(c[k].fd);c[k].fd=-1;break;}}}
 }
 abort_write("сервер завершает работу");log_event("[SERVER] stop version=%d reads=%lld publications=%lld stages=%lld aborted=%lld",version,reads,pubs,stages_done,aborted);for(int i=0;i<MAXC;i++)if(c[i].fd>=0)close(c[i].fd);close(fd);if(log_file)fclose(log_file);return 0;
}
static int sendcmd(int fd,const char *s,char *ans){char b[LINE];snprintf(b,sizeof(b),"%s\n",s);if(send(fd,b,strlen(b),MSG_NOSIGNAL)<0)return 0;size_t n=0;while(n+1<LINE){char ch;ssize_t z=recv(fd,&ch,1,0);if(z<=0)return 0;if(ch=='\n'){ans[n]=0;return 1;}if(ch!='\r')ans[n++]=ch;}return 0;}
static int client_main(const char *role,int argc,char **argv){const char *host="127.0.0.1",*port="5050";int id=1,count=-1;unsigned seed=42;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--host")&&i+1<argc)host=argv[++i];else if(!strcmp(argv[i],"--port")&&i+1<argc)port=argv[++i];else if(!strcmp(argv[i],"--id")&&i+1<argc)id=atoi(argv[++i]);else if(!strcmp(argv[i],"--ops")&&i+1<argc)count=atoi(argv[++i]);else if(!strcmp(argv[i],"--seed")&&i+1<argc)seed=(unsigned)atoi(argv[++i]);else return 2;}struct addrinfo_dummy{int unused;};(void)sizeof(struct addrinfo_dummy);struct sockaddr_in a={0};int fd=socket(AF_INET,SOCK_STREAM,0);a.sin_family=AF_INET;a.sin_port=htons((unsigned short)atoi(port));if(fd<0||inet_pton(AF_INET,host,&a.sin_addr)!=1||connect(fd,(struct sockaddr*)&a,sizeof(a))<0){perror("connect");return 1;}char ans[LINE],cmd[LINE];snprintf(cmd,sizeof(cmd),"HELLO %s %d",role,id);if(!sendcmd(fd,cmd,ans)||strncmp(ans,"WELCOME ",8)){fprintf(stderr,"Handshake failed\n");close(fd);return 1;}int ignored_version=0,ignored_strategy=0,size=5,st=3,defops=5,rd=100,prep=100,confirm=100;sscanf(ans,"WELCOME %d %d %d %d %d %d %d %d",&ignored_version,&size,&ignored_strategy,&st,&defops,&rd,&prep,&confirm);if(count<0)count=defops;signal(SIGINT,on_signal);signal(SIGTERM,on_signal);printf("[%s %d] connected %s:%s\n",role,id,host,port);
 if(!strcmp(role,"observer")){int done=0;while(!stopping&&(count==0||count<0||done<count)){if(!sendcmd(fd,"STATUS",ans))break;puts(ans);done++;sleep(1);}}
 else if(!strcmp(role,"reader")){for(int i=0;!stopping&&(count==0||i<count);i++){snprintf(cmd,sizeof(cmd),"READ %u",seed++%(unsigned)size);if(!sendcmd(fd,cmd,ans))break;printf("[Reader %d] %s\n",id,ans);usleep(100000);}}
 else {for(int op=0;!stopping&&(count==0||op<count);op++){snprintf(cmd,sizeof(cmd),"BEGIN %u",seed++);if(!sendcmd(fd,cmd,ans)||strncmp(ans,"OK BEGIN",8)){puts(ans);usleep(100000);continue;}for(int j=0;j<st&&!stopping;j++){usleep((useconds_t)prep*1000);snprintf(cmd,sizeof(cmd),"STAGE %d",j);if(!sendcmd(fd,cmd,ans)||strncmp(ans,"OK STAGE",8)){puts(ans);break;}}if(stopping){sendcmd(fd,"ABORT",ans);break;}usleep((useconds_t)confirm*1000);if(!sendcmd(fd,"PUBLISH",ans))break;printf("[Writer %d] %s\n",id,ans);}}
 (void)sendcmd(fd,"QUIT",ans);close(fd);return 0;
}
int main(int argc,char **argv){if(argc<2){fprintf(stderr,"Usage: %s server|reader|writer|observer [options]\n",argv[0]);return 2;}if(!strcmp(argv[1],"server"))return server_main(argc-1,argv+1);if(!strcmp(argv[1],"reader")||!strcmp(argv[1],"writer")||!strcmp(argv[1],"observer"))return client_main(argv[1],argc-1,argv+1);return 2;}
