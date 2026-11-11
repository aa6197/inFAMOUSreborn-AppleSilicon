/* test_spu_helpers_vec.c -- spu_helpers_vec.h (whole-register forms used by the rc sources) against the
 * scalar references in spu_helpers.h / spu_context.h: every redirected integer helper on random and
 * edge-case registers, every immediate in its field range, and the LS read/write byte order.
 * Exit status 0 = all equal. Prints "N checks, M differ"; M must be 0.
 *
 * Build from ps3recomp/ (x86 needs -msse4.1; arm64 uses the sse2neon path):
 *   cc -O2 -std=gnu17 -fno-strict-aliasing -w -I runtime/spu -I include -I runtime/ppu -I runtime/memory \
 *      -I runtime/syscalls -I runtime/platform runtime/spu/tests/test_spu_helpers_vec.c -o test_spu_helpers_vec
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "spu_helpers.h"
/* the LS watch hook of spu_context.h lives in spu_channels.c: off here */
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)lsa; (void)w; (void)p; (void)pc; (void)lr; }

static u128 R_xsbh(u128 a){return spu_xsbh(a);}
static u128 R_xshw(u128 a){return spu_xshw(a);}
static u128 R_a(u128 a,u128 b){return spu_a(a,b);}
static u128 R_sf(u128 a,u128 b){return spu_sf(a,b);}
static u128 R_ah(u128 a,u128 b){return spu_ah(a,b);}
static u128 R_sfh(u128 a,u128 b){return spu_sfh(a,b);}
static u128 R_cg(u128 a,u128 b){return spu_cg(a,b);}
static u128 R_bg(u128 a,u128 b){return spu_bg(a,b);}
static u128 R_and(u128 a,u128 b){return spu_and(a,b);}
static u128 R_or(u128 a,u128 b){return spu_or(a,b);}
static u128 R_xor(u128 a,u128 b){return spu_xor(a,b);}
static u128 R_andc(u128 a,u128 b){return spu_andc(a,b);}
static u128 R_orc(u128 a,u128 b){return spu_orc(a,b);}
static u128 R_nor(u128 a,u128 b){return spu_nor(a,b);}
static u128 R_nand(u128 a,u128 b){return spu_nand(a,b);}
static u128 R_eqv(u128 a,u128 b){return spu_eqv(a,b);}
static u128 R_ceq(u128 a,u128 b){return spu_ceq(a,b);}
static u128 R_ceqh(u128 a,u128 b){return spu_ceqh(a,b);}
static u128 R_ceqb(u128 a,u128 b){return spu_ceqb(a,b);}
static u128 R_cgt(u128 a,u128 b){return spu_cgt(a,b);}
static u128 R_clgt(u128 a,u128 b){return spu_clgt(a,b);}
static u128 R_clgth(u128 a,u128 b){return spu_clgth(a,b);}
static u128 R_mpy(u128 a,u128 b){return spu_mpy(a,b);}
static u128 R_mpyu(u128 a,u128 b){return spu_mpyu(a,b);}
static u128 R_mpyh(u128 a,u128 b){return spu_mpyh(a,b);}
static u128 R_mpyhh(u128 a,u128 b){return spu_mpyhh(a,b);}
static u128 R_mpys(u128 a,u128 b){return spu_mpys(a,b);}
static u128 R_addx(u128 a,u128 b,u128 c){return spu_addx(a,b,c);}
static u128 R_sfx(u128 a,u128 b,u128 c){return spu_sfx(a,b,c);}
static u128 R_selb(u128 a,u128 b,u128 c){return spu_selb(a,b,c);}
static u128 R_mpya(u128 a,u128 b,u128 c){return spu_mpya(a,b,c);}
static u128 R_ai(u128 a,int imm){return spu_ai(a,imm);}
static u128 R_sfi(u128 a,int imm){return spu_sfi(a,imm);}
static u128 R_ahi(u128 a,int imm){return spu_ahi(a,imm);}
static u128 R_sfhi(u128 a,int imm){return spu_sfhi(a,imm);}
static u128 R_andi(u128 a,int imm){return spu_andi(a,imm);}
static u128 R_ori(u128 a,int imm){return spu_ori(a,imm);}
static u128 R_xori(u128 a,int imm){return spu_xori(a,imm);}
static u128 R_andhi(u128 a,int imm){return spu_andhi(a,imm);}
static u128 R_orhi(u128 a,int imm){return spu_orhi(a,imm);}
static u128 R_xorhi(u128 a,int imm){return spu_xorhi(a,imm);}
static u128 R_andbi(u128 a,int imm){return spu_andbi(a,imm);}
static u128 R_orbi(u128 a,int imm){return spu_orbi(a,imm);}
static u128 R_xorbi(u128 a,int imm){return spu_xorbi(a,imm);}
static u128 R_ceqi(u128 a,int imm){return spu_ceqi(a,imm);}
static u128 R_ceqhi(u128 a,int imm){return spu_ceqhi(a,imm);}
static u128 R_ceqbi(u128 a,int imm){return spu_ceqbi(a,imm);}
static u128 R_cgti(u128 a,int imm){return spu_cgti(a,imm);}
static u128 R_cgthi(u128 a,int imm){return spu_cgthi(a,imm);}
static u128 R_cgtbi(u128 a,int imm){return spu_cgtbi(a,imm);}
static u128 R_clgti(u128 a,int imm){return spu_clgti(a,imm);}
static u128 R_clgthi(u128 a,int imm){return spu_clgthi(a,imm);}
static u128 R_clgtbi(u128 a,int imm){return spu_clgtbi(a,imm);}
static u128 R_shli(u128 a,int imm){return spu_shli(a,imm);}
static u128 R_shlhi(u128 a,int imm){return spu_shlhi(a,imm);}
static u128 R_rotmi(u128 a,int imm){return spu_rotmi(a,imm);}
static u128 R_rotmai(u128 a,int imm){return spu_rotmai(a,imm);}
static u128 R_rothmi(u128 a,int imm){return spu_rothmi(a,imm);}
static u128 R_rotmhi(u128 a,int imm){return spu_rotmhi(a,imm);}
static u128 R_rotmahi(u128 a,int imm){return spu_rotmahi(a,imm);}
static u128 R_roti(u128 a,int imm){return spu_roti(a,imm);}
static u128 R_mpyi(u128 a,int imm){return spu_mpyi(a,imm);}
static u128 R_mpyui(u128 a,int imm){return spu_mpyui(a,imm);}
static u128 R_splat_u32(int v){return spu_splat_u32(v);}
static u128 R_il(int v){return spu_il(v);}
static u128 R_ila(int v){return spu_ila(v);}
static u128 R_ilhu(int v){return spu_ilhu(v);}
static u128 R_ilh(int v){return spu_ilh(v);}
static u128 R_link(int v){return spu_link(v);}
static u128 R_pref_u32(int v){return spu_pref_u32(v);}
static u128 R_iohl(u128 a,int v){return spu_iohl(a,v);}
static u128 R_fsm(u128 a){return spu_fsm(a);}
static u128 spu_ls_read128_ref_(const spu_context* c,uint32_t lsa){return spu_ls_read128(c,lsa);}
static void spu_ls_write128_ref_(spu_context* c,uint32_t lsa,u128 v){spu_ls_write128(c,lsa,v);}
#include "spu_helpers_vec.h"
#if !defined(spu_a)
int main(void) { printf("vec helpers: not enabled for this target (needs SSE4.1 or arm64 NEON)\n"); return 0; }
#else

static uint64_t s=88172645463325252ull; static uint32_t rnd(void){s^=s<<13;s^=s>>7;s^=s<<17;return (uint32_t)s;}
static const uint32_t E[]={0,1,2,0xFFFFFFFFu,0xFFFFFFFEu,0x7FFFFFFFu,0x80000000u,0x80000001u,0xFFFFu,0x10000u,0x8000u,0x7FFFu,0xFFFF8000u,0x00007FFFu,0xFFu,0x80u,0x7Fu,0x7F7F7F7Fu,0x80808080u,0xFF00FF00u,0x00FF00FFu,0x8000FFFFu,0xFFFF0000u};
static u128 rv(void){u128 r; for(int i=0;i<4;i++){uint32_t k=rnd(); r._u32[i]=(k&3)==0?E[rnd()%(sizeof E/4)]:(k&3)==1?(rnd()&0xFFFF)*((rnd()&1)?0x10001u:1u):rnd();} return r;}
static long fails=0,checks=0;
static void cmp(const char*n,u128 x,u128 y,u128 a,int imm){checks++; if(memcmp(&x,&y,16)){ if(fails<20) printf("DIFF %s imm=%d a=%08x %08x %08x %08x ref=%08x %08x %08x %08x vec=%08x %08x %08x %08x\n",n,imm,a._u32[0],a._u32[1],a._u32[2],a._u32[3],x._u32[0],x._u32[1],x._u32[2],x._u32[3],y._u32[0],y._u32[1],y._u32[2],y._u32[3]); fails++;}}
int main(void){
 const int N=200000;
 for(int it=0;it<N;it++){ u128 a=rv(),b=rv(),c=rv();
  cmp("xsbh",R_xsbh(a),spu__v_xsbh(a),a,0);
  cmp("xshw",R_xshw(a),spu__v_xshw(a),a,0);
  cmp("a",R_a(a,b),spu__v_a(a,b),a,0); cmp("a/aa",R_a(a,a),spu__v_a(a,a),a,0);
  cmp("sf",R_sf(a,b),spu__v_sf(a,b),a,0); cmp("sf/aa",R_sf(a,a),spu__v_sf(a,a),a,0);
  cmp("ah",R_ah(a,b),spu__v_ah(a,b),a,0); cmp("ah/aa",R_ah(a,a),spu__v_ah(a,a),a,0);
  cmp("sfh",R_sfh(a,b),spu__v_sfh(a,b),a,0); cmp("sfh/aa",R_sfh(a,a),spu__v_sfh(a,a),a,0);
  cmp("cg",R_cg(a,b),spu__v_cg(a,b),a,0); cmp("cg/aa",R_cg(a,a),spu__v_cg(a,a),a,0);
  cmp("bg",R_bg(a,b),spu__v_bg(a,b),a,0); cmp("bg/aa",R_bg(a,a),spu__v_bg(a,a),a,0);
  cmp("and",R_and(a,b),spu__v_and(a,b),a,0); cmp("and/aa",R_and(a,a),spu__v_and(a,a),a,0);
  cmp("or",R_or(a,b),spu__v_or(a,b),a,0); cmp("or/aa",R_or(a,a),spu__v_or(a,a),a,0);
  cmp("xor",R_xor(a,b),spu__v_xor(a,b),a,0); cmp("xor/aa",R_xor(a,a),spu__v_xor(a,a),a,0);
  cmp("andc",R_andc(a,b),spu__v_andc(a,b),a,0); cmp("andc/aa",R_andc(a,a),spu__v_andc(a,a),a,0);
  cmp("orc",R_orc(a,b),spu__v_orc(a,b),a,0); cmp("orc/aa",R_orc(a,a),spu__v_orc(a,a),a,0);
  cmp("nor",R_nor(a,b),spu__v_nor(a,b),a,0); cmp("nor/aa",R_nor(a,a),spu__v_nor(a,a),a,0);
  cmp("nand",R_nand(a,b),spu__v_nand(a,b),a,0); cmp("nand/aa",R_nand(a,a),spu__v_nand(a,a),a,0);
  cmp("eqv",R_eqv(a,b),spu__v_eqv(a,b),a,0); cmp("eqv/aa",R_eqv(a,a),spu__v_eqv(a,a),a,0);
  cmp("ceq",R_ceq(a,b),spu__v_ceq(a,b),a,0); cmp("ceq/aa",R_ceq(a,a),spu__v_ceq(a,a),a,0);
  cmp("ceqh",R_ceqh(a,b),spu__v_ceqh(a,b),a,0); cmp("ceqh/aa",R_ceqh(a,a),spu__v_ceqh(a,a),a,0);
  cmp("ceqb",R_ceqb(a,b),spu__v_ceqb(a,b),a,0); cmp("ceqb/aa",R_ceqb(a,a),spu__v_ceqb(a,a),a,0);
  cmp("cgt",R_cgt(a,b),spu__v_cgt(a,b),a,0); cmp("cgt/aa",R_cgt(a,a),spu__v_cgt(a,a),a,0);
  cmp("clgt",R_clgt(a,b),spu__v_clgt(a,b),a,0); cmp("clgt/aa",R_clgt(a,a),spu__v_clgt(a,a),a,0);
  cmp("clgth",R_clgth(a,b),spu__v_clgth(a,b),a,0); cmp("clgth/aa",R_clgth(a,a),spu__v_clgth(a,a),a,0);
  cmp("mpy",R_mpy(a,b),spu__v_mpy(a,b),a,0); cmp("mpy/aa",R_mpy(a,a),spu__v_mpy(a,a),a,0);
  cmp("mpyu",R_mpyu(a,b),spu__v_mpyu(a,b),a,0); cmp("mpyu/aa",R_mpyu(a,a),spu__v_mpyu(a,a),a,0);
  cmp("mpyh",R_mpyh(a,b),spu__v_mpyh(a,b),a,0); cmp("mpyh/aa",R_mpyh(a,a),spu__v_mpyh(a,a),a,0);
  cmp("mpyhh",R_mpyhh(a,b),spu__v_mpyhh(a,b),a,0); cmp("mpyhh/aa",R_mpyhh(a,a),spu__v_mpyhh(a,a),a,0);
  cmp("mpys",R_mpys(a,b),spu__v_mpys(a,b),a,0); cmp("mpys/aa",R_mpys(a,a),spu__v_mpys(a,a),a,0);
  cmp("addx",R_addx(a,b,c),spu__v_addx(a,b,c),a,0);
  cmp("sfx",R_sfx(a,b,c),spu__v_sfx(a,b,c),a,0);
  cmp("selb",R_selb(a,b,c),spu__v_selb(a,b,c),a,0);
  cmp("mpya",R_mpya(a,b,c),spu__v_mpya(a,b,c),a,0);
  cmp("fsm",R_fsm(a),spu__v_fsm(a),a,0);
 }
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ai",R_ai(a,imm),spu__v_ai(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("sfi",R_sfi(a,imm),spu__v_sfi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ahi",R_ahi(a,imm),spu__v_ahi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("sfhi",R_sfhi(a,imm),spu__v_sfhi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("andi",R_andi(a,imm),spu__v_andi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ori",R_ori(a,imm),spu__v_ori(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("xori",R_xori(a,imm),spu__v_xori(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("andhi",R_andhi(a,imm),spu__v_andhi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("orhi",R_orhi(a,imm),spu__v_orhi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("xorhi",R_xorhi(a,imm),spu__v_xorhi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("andbi",R_andbi(a,imm),spu__v_andbi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("orbi",R_orbi(a,imm),spu__v_orbi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("xorbi",R_xorbi(a,imm),spu__v_xorbi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ceqi",R_ceqi(a,imm),spu__v_ceqi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ceqhi",R_ceqhi(a,imm),spu__v_ceqhi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("ceqbi",R_ceqbi(a,imm),spu__v_ceqbi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("cgti",R_cgti(a,imm),spu__v_cgti(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("cgthi",R_cgthi(a,imm),spu__v_cgthi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("cgtbi",R_cgtbi(a,imm),spu__v_cgtbi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("clgti",R_clgti(a,imm),spu__v_clgti(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("clgthi",R_clgthi(a,imm),spu__v_clgthi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("clgtbi",R_clgtbi(a,imm),spu__v_clgtbi(a,(int32_t)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("shli",R_shli(a,imm),spu__v_shli(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("shlhi",R_shlhi(a,imm),spu__v_shlhi(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("rotmi",R_rotmi(a,imm),spu__v_rotmi(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("rotmai",R_rotmai(a,imm),spu__v_rotmai(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("rothmi",R_rothmi(a,imm),spu__v_rothmi(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("rotmhi",R_rotmhi(a,imm),spu__v_rotmhi(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("rotmahi",R_rotmahi(a,imm),spu__v_rotmahi(a,(int)imm),a,imm);}
 for(int imm=-128;imm<=127;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("roti",R_roti(a,imm),spu__v_roti(a,(int)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("mpyi",R_mpyi(a,imm),spu__v_mpyi(a,(int32_t)imm),a,imm);}
 for(int imm=-1024;imm<=1023;imm++) for(int k=0;k<200;k++){u128 a=rv(); cmp("mpyui",R_mpyui(a,imm),spu__v_mpyui(a,(int32_t)imm),a,imm);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("splat_u32",R_splat_u32(v),spu__v_splat_u32(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("il",R_il(v),spu__v_il(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("ila",R_ila(v),spu__v_ila(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("ilhu",R_ilhu(v),spu__v_ilhu(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("ilh",R_ilh(v),spu__v_ilh(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("link",R_link(v),spu__v_link(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<140000)?(k-70000):(int)rnd(); u128 z; memset(&z,0,16); cmp("pref_u32",R_pref_u32(v),spu__v_pref_u32(v),z,v);}
 for(int k=0;k<300000;k++){int v=(k<70000)?k:(int)rnd(); u128 a=rv(); cmp("iohl",R_iohl(a,v),spu__v_iohl(a,(uint16_t)v),a,v);}
 /* LS read/write vs spu_context.h references */
 { static uint8_t ls[SPU_LS_MASK+1+16]; spu_context* ctx=(spu_context*)calloc(1,sizeof(spu_context)); ctx->ls=ls;
   for(int k=0;k<200000;k++){ for(int j=0;j<16;j++) ls[(k*16+j)&SPU_LS_MASK]=(uint8_t)rnd(); uint32_t lsa=rnd();
     u128 x=spu_ls_read128_ref_(ctx,lsa), y=spu__v_ls_rd(ls,lsa); cmp("ls_rd",x,y,x,(int)lsa);
     u128 v=rv(); static uint8_t l2[SPU_LS_MASK+1]; memcpy(l2,ls,SPU_LS_MASK+1); spu_ls_write128_ref_(ctx,lsa,v); spu__v_ls_wr(l2,lsa,v);
     checks++; if(memcmp(l2,ls,SPU_LS_MASK+1)){ if(fails<20) printf("DIFF ls_wr lsa=%08x\n",lsa); fails++; } } }
 printf("vec helpers: %ld checks, %ld differ (2 V1, 25 V2, 4 V3, 32 VI)\n",checks,fails);
 return fails?1:0; }
#endif
