#include "harness.h"
#include <math.h>
#define PIN(n,a,b) ((n)<(a)?(a):(n)>(b)?(b):(n))
#define csmemset memset
typedef struct {real yaw,pitch;} real_euler_angles2d;
struct unit_control_data {real_vector3d looking_vector,facing_vector,aiming_vector;};
struct observer_command {long flags;real_vector3d focus_position,focus_offset;real focus_distance,field_of_view;real_vector3d forward,up,focus_velocity;};
struct player_datum {long unit_index;short local_player_index;};
struct unit_datum {struct {long parent_object_index,damage_flags;}object;struct {short current_zoom_level;}unit;};
enum {_object_dead_bit,_observer_command_valid_bit};
static struct player_datum player={1,0};static struct unit_datum unit={{NONE,0},{NONE}};
static boolean has_player=TRUE,has_unit=TRUE,multiplayer,cinematic,enabled,alt;
static int key;static float wheel;static real_vector3d direction={1,0,0};
static long local_player_get_player_index(short n){return has_player?0:NONE;}
static struct player_datum *player_try_and_get(long n){return n==0&&has_player?&player:NULL;}
static struct unit_datum *unit_try_and_get(long n){return n==1&&has_unit?&unit:NULL;}
static int platform_third_person_key(void){int k=key;key=0;return k;}
static void platform_third_person_active(int a){enabled=a;}
static int platform_free_look_held(void){return alt;}
static float platform_third_person_wheel(void){real r=wheel;wheel=0;return r;}
static boolean game_engine_running(void){return multiplayer;}
static boolean cinematic_in_progress(void){return cinematic;}
static void player_control_get_facing_direction(short n,real_vector3d *v){*v=direction;}
static real normalize3d(real_vector3d *v){return sqrtf(v->i*v->i+v->j*v->j+v->k*v->k);}
static void unit_get_camera_position(long n,real_vector3d *v){*v=(real_vector3d){10,20,30};}
static real player_control_get_field_of_view(short n){return 1.2f;}
static void observer_up_from_forward(real_vector3d const *v,real_vector3d *up){*up=(real_vector3d){0,0,1};}
static void object_get_velocities(long n,real_vector3d *v,void *angular){*v=(real_vector3d){1,2,3};}
static void player_aiming_vector_from_facing(long n,real_vector3d *v,real_euler_angles2d const *f){*v=(real_vector3d){cosf(f->yaw),sinf(f->yaw),f->pitch};}
typedef short director_perspective;
typedef void (*director_camera_update_proc)(void*,void*,void*);
struct director {director_camera_update_proc camera_proc;short seat_state;byte camera_data[128];};
struct first_person_camera {int unused;};struct following_camera {int unused;};
static void first_person_camera_update(void*a,void*b,void*c){}
static void following_camera_update(void*a,void*b,void*c){}
static void first_person_camera_new(struct first_person_camera *p){}
static void following_camera_new(struct following_camera *p){}
static struct director director={first_person_camera_update,0,{0}};static short following;
static struct director *director_get(short n){return &director;}
static long player_control_get_unit_index(short n){return player.unit_index;}
static short director_desired_perspective(long n,director_perspective *p){*p=following;return following;}
static void director_set_camera(short n,director_camera_update_proc proc,boolean transition){director.camera_proc=proc;}
#include "under_test.inc"
static void begin_free_look(void){key=1;third_person_camera_wanted(0);alt=TRUE;third_person_camera_wanted(0);}
static real_euler_angles2d action_facing(real yaw,short local)
{
 real_euler_angles2d facing={yaw,0};
 direction=(real_vector3d){cosf(yaw),sinf(yaw),0};
#if HAS_ACTION_FILTER
 third_person_action_facing(local,&facing);
#endif
 return facing;
}
static struct unit_control_data consume(real_euler_angles2d facing,short local)
{
 struct unit_control_data c={0};player.local_player_index=local;
 player_aiming_vector_from_facing(0,&c.aiming_vector,&facing);
 c.looking_vector=c.facing_vector=c.aiming_vector;
 third_person_control(0,&facing,&c);return c;
}
int main(int argc,char **argv){char const *case_name=argc>1?argv[1]:"";
 CASE("replicas"){
  begin_free_look();consume(action_facing(0,0),0);
  real_euler_angles2d packet=action_facing(1,0);
  struct unit_control_data predicted=consume(packet,0),host=consume(packet,NONE);
  CHECK(fabsf(predicted.aiming_vector.i-host.aiming_vector.i)<1e-6f&&fabsf(predicted.aiming_vector.j-host.aiming_vector.j)<1e-6f,"host and predicted aim disagree during free look");
  CHECK(packet.yaw==0,"camera angle was sent as body aim");
 }
 else CASE("head"){begin_free_look();consume(action_facing(0,0),0);struct unit_control_data c=consume(action_facing(1,0),0);CHECK(fabsf(c.looking_vector.j-sinf(1))<1e-6f&&c.aiming_vector.i==1,"head stopped following camera independently of held aim");}
 else CASE("normal"){real_euler_angles2d a=action_facing(1,0);CHECK(a.yaw==1,"first-person aim changed");}
 else CASE("second-player"){begin_free_look();consume(action_facing(0,0),0);real_euler_angles2d a=action_facing(1,1);CHECK(a.yaw==1,"second local player's input changed");}
 else CASE("release"){begin_free_look();consume(action_facing(0,0),0);alt=FALSE;third_person_camera_wanted(0);real_euler_angles2d a=action_facing(2,0);CHECK(a.yaw==2,"aim did not return to camera on release");}
 else return 2;return 0;
}
