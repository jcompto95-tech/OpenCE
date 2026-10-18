#include "harness.h"
#include "config.inc"
typedef short director_perspective;
typedef void (*director_camera_update_proc)(void *, void *, void *);
struct director {director_camera_update_proc camera_proc; real camera_change_pause; short perspective;};
static struct director views[2];
static struct weapon_sound {long weapon_index, current_sound_index; short state, current_sound_state;} weapon;
static int played;
static void first_person_camera_update(void *a,void *b,void *c){}
static void third_person_camera_update(void *a,void *b,void *c){}
static void following_camera_update(void *a,void *b,void *c){}
static void scripted_camera_update(void *a,void *b,void *c){}
static void dead_camera_update(void *a,void *b,void *c){}
static void flying_camera_update(void *a,void *b,void *c){}
static struct director *director_get(short local){CHECK(local>=0&&local<2,"wrong local view");return &views[local];}
static int global_origin3d,global_forward3d;
static long object_impulse_sound_new(long object,long sound,long unused,int origin,int forward,real volume)
{
 CHECK(object==123&&sound==456&&volume==1.f,"sound source or volume changed");
 return ++played;
}
#include "under_test.inc"
int main(int argc,char **argv)
{
 char const *case_name=argc>1?argv[1]:"";
 int expected=0;
 weapon.weapon_index=123;weapon.state=7;
 weapon.current_sound_index=NONE;weapon.current_sound_state=NONE;
 views[0].camera_proc=first_person_camera_update;
 if(!strcmp(case_name,"first"))expected=1;
 else if(!strcmp(case_name,"third")){views[0].camera_proc=third_person_camera_update;expected=1;}
 else if(!strcmp(case_name,"vehicle"))views[0].camera_proc=following_camera_update;
 else if(!strcmp(case_name,"death"))views[0].camera_proc=dead_camera_update;
 else if(!strcmp(case_name,"scripted"))views[0].camera_proc=scripted_camera_update;
 else if(!strcmp(case_name,"debug"))views[0].camera_proc=flying_camera_update;
 else if(!strcmp(case_name,"no-event")){
  views[0].camera_proc=third_person_camera_update;event(0,NONE);
  CHECK(!played&&weapon.current_sound_index==NONE,"invented an animation sound");return 0;
 }
 else if(!strcmp(case_name,"secondary")){
  views[0].camera_proc=third_person_camera_update;views[1].camera_proc=following_camera_update;
  event(1,456);CHECK(!played,"another view inherited the first view's sound policy");
  views[1].camera_proc=first_person_camera_update;event(1,456);
  CHECK(played==1,"second local first-person view lost sound");return 0;
 }
 else if(!strcmp(case_name,"transitions")){
  int frame;
  for(frame=0;frame<8;frame++){
   if(frame==2)views[0].camera_proc=third_person_camera_update;
   if(frame==4)views[0].camera_proc=first_person_camera_update;
   event(0,(frame==2||frame==5)?456:NONE);
  }
  CHECK(played==2&&weapon.current_sound_index==2&&weapon.current_sound_state==7,"lost or repeated reload events across camera changes");
  views[0].camera_proc=following_camera_update;event(0,456);
  views[0].camera_proc=scripted_camera_update;event(0,456);
  CHECK(played==2,"stale third-person state leaked audio into another camera");return 0;
 }
 else return 2;
 event(0,456);
 CHECK(played==expected,"wrong playback policy");
 CHECK(weapon.current_sound_index==(expected?1:NONE)&&weapon.current_sound_state==(expected?7:NONE),"sound lifecycle changed");
 return 0;
}
