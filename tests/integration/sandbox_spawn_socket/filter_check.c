/* Every seccomp filter the kernel layer can build, for each mix of the
 * fork, exec and network grants and 0 to 2 allowed execve pointers, is
 * well formed: every jump lands inside it and it ends in a return. (The
 * kernel would refuse a bad one with EINVAL, and the child would not start.)
 * Built by test_sandbox_kernel_layer.sh with -I at the source tree. */
#include <stdio.h>
int list_size(void* l){(void)l;return 0;}
void* list_get_raw(void* l,int i){(void)l;(void)i;return 0;}
const char* aether_string_data(const void* p){return p;}
char* aether_spawn_resolve(const char* p){(void)p;return 0;}
#include "runtime/sandbox/aether_sandbox_kernel_linux.c"   /* the filter builder itself */
int main(void){
 int bad=0;
 for(int m=0;m<8;m++) for(int np=0;np<=2;np++){
  SbxPolicy p; memset(&p,0,sizeof p); p.fork=m&1;p.exec=!!(m&2);p.net=!!(m&4); p.ptrs[0]=0x55551234abcdULL; p.ptrs[1]=0x7fff00001111ULL; p.nptrs=np;
  struct sock_filter f[AETHER_SBX_FILTER_MAX]; int n=build_filter(f,AETHER_SBX_FILTER_MAX,&p);
  if(n<0){printf("m=%d np=%d n<0\n",m,np);bad++;continue;}
  for(int i=0;i<n;i++){ unsigned c=f[i].code; if(BPF_CLASS(c)==BPF_JMP){ int t=i+1+f[i].jt, e=i+1+f[i].jf; if(t>=n||e>=n){ printf("BAD m=%d np=%d %d -> %d/%d\n",m,np,i,t,e);bad++;}} }
  if (BPF_CLASS(f[n-1].code)!=BPF_RET) {printf("last not RET\n");bad++;}
  if(m==0&&np==2) printf("n=%d\n",n);
 }
 printf("bad=%d\n",bad); return bad;}
