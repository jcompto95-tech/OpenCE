#include "harness.h"
#define SET_FLAG(v,b,on) ((v)=((v)&~FLAG(b))|((on)?FLAG(b):0))
#define csmemset memset
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define TICKS_PER_SECOND 30
typedef struct {real yaw,pitch,roll;} real_euler_angles3d;
#include "config.inc"
typedef void (*director_camera_update_proc)(void*,void*,void*);
struct director {director_camera_update_proc camera_proc;boolean debug_controls;real debug_input_scale;struct {real delta;}debug_variables[4];};
struct gamepad_state {byte buttons[NUMBER_OF_GAMEPAD_BUTTONS];struct {short x,y;}sticks[2];};
struct mouse_state {byte buttons[4];int x,y,wheel_delta;};
static struct director view;static struct gamepad_state pad;
static struct {real dtime;}director_globals={1.f/60};
static struct {short local_player_index;}player;
static boolean director_camera_switch_fast;static int input_inhibited,facing_inhibited;
static void first_person_camera_update(void*a,void*b,void*c){}
static void following_camera_update(void*a,void*b,void*c){}
static void third_person_camera_update(void*a,void*b,void*c){}
static void flying_camera_update(void*a,void*b,void*c){}
static struct director *director_get(short n){return &view;}
static long local_player_get_player_index(short n){return 0;}
static typeof(player) *player_get(long n){return &player;}
static boolean input_has_gamepad(short n){return TRUE;}
static struct gamepad_state *input_get_gamepad_state(short n){return &pad;}
static struct mouse_state *input_get_mouse_state(void){return NULL;}
static boolean input_key_is_down(short n){return FALSE;}
static void director_process_variables(short n,unsigned long flags,real delta){}
static void director_inhibit_input(short n){input_inhibited++;}
static void director_inhibit_facing(short n){facing_inhibited++;}
#include "under_test.inc"
int main(int argc,char **argv){char const *case_name=argc>1?argv[1]:"";struct camera_control controls;
 if(!strcmp(case_name,"third-person"))view.camera_proc=third_person_camera_update;
 else if(!strcmp(case_name,"first-person"))view.camera_proc=first_person_camera_update;
 else if(!strcmp(case_name,"vehicle"))view.camera_proc=following_camera_update;
 else if(!strcmp(case_name,"debug-camera"))view.camera_proc=flying_camera_update;
 else return 2;
 pad.buttons[_gamepad_binary_button_right_thumb]=1;
 director_update_controls(0,&controls);
 if(!strcmp(case_name,"debug-camera"))CHECK(view.debug_controls&&input_inhibited&&facing_inhibited,"real debug camera controls stopped working");
 else CHECK(!view.debug_controls&&!input_inhibited&&!facing_inhibited&&!controls.active,"gameplay camera captured debug controls");
 return 0;
}
