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
struct director {director_camera_update_proc camera_proc;short seat_state;byte camera_data[128];real debug_input_scale,camera_change_pause;boolean debug_controls;};
struct first_person_camera {int unused;};struct following_camera {int unused;};
static void first_person_camera_update(void*a,void*b,void*c){}
static void following_camera_update(void*a,void*b,void*c){}
static void first_person_camera_new(struct first_person_camera *p){}
static void following_camera_new(struct following_camera *p){}
static struct director director={.camera_proc=first_person_camera_update};static short following;
static struct director *director_get(short n){return &director;}
static long player_control_get_unit_index(short n){return player.unit_index;}
static short director_desired_perspective(long n,director_perspective *p){*p=following;return following;}
static void scripted_camera_update(void*a,void*b,void*c){}
#include "under_test.inc"
static void on(void){key=1;CHECK(third_person_camera_wanted(0),"not enabled");}
int main(int argc,char **argv){char const *case_name=argc>1?argv[1]:"";
 CASE("toggle"){CHECK(!third_person_camera_wanted(0),"default on");on();CHECK(third_person_camera_wanted(0),"key repeated");key=1;CHECK(!third_person_camera_wanted(0),"toggle off");}
 else CASE("eligibility"){on();CHECK(!third_person_camera_wanted(1),"second local player");multiplayer=TRUE;CHECK(!third_person_camera_wanted(0)&&!enabled,"competitive allowed");multiplayer=FALSE;cinematic=TRUE;CHECK(!third_person_camera_wanted(0),"cutscene");cinematic=FALSE;unit.object.damage_flags=FLAG(_object_dead_bit);CHECK(!third_person_camera_wanted(0),"dead");unit.object.damage_flags=0;has_unit=FALSE;CHECK(!third_person_camera_wanted(0),"missing unit");has_unit=TRUE;has_player=FALSE;CHECK(!third_person_camera_wanted(0),"missing player");}
 else CASE("scope"){on();unit.unit.current_zoom_level=0;CHECK(!third_person_camera_wanted(0)&&!enabled,"scoped third person");unit.unit.current_zoom_level=NONE;CHECK(third_person_camera_wanted(0),"scope exit");}
 else CASE("distance"){short n=0;struct observer_command cmd;on();wheel=100;third_person_camera_update(NULL,&n,&cmd);CHECK(cmd.focus_distance==THIRD_PERSON_NEAREST,"near clamp");wheel=-100;third_person_camera_update(NULL,&n,&cmd);CHECK(cmd.focus_distance==THIRD_PERSON_FURTHEST,"far clamp");}
 else CASE("camera"){short n=0;struct observer_command cmd;on();third_person_camera_update(NULL,&n,&cmd);CHECK(TEST_FLAG(cmd.flags,_observer_command_valid_bit)&&cmd.focus_position.k==30&&cmd.focus_offset.j==THIRD_PERSON_RIGHT&&cmd.field_of_view==1.2f,"observer command");}
 else CASE("free-look"){real_euler_angles2d f={0,0};struct unit_control_data c={0};on();alt=TRUE;third_person_camera_wanted(0);third_person_control(0,&f,&c);f.yaw=1;c.aiming_vector=(real_vector3d){0,1,0};third_person_control(0,&f,&c);CHECK(c.aiming_vector.i==1&&c.looking_vector.j==1,"free look changed aim");alt=FALSE;third_person_camera_wanted(0);CHECK(!third_person_frozen,"frozen after release");}
 else CASE("vehicle-transition"){on();alt=TRUE;director_choose_game_perspective(0,FALSE);CHECK(director.camera_proc==third_person_camera_update,"did not enter camera");unit.object.parent_object_index=2;following=TRUE;director_choose_game_perspective(0,FALSE);CHECK(!enabled&&!third_person_free_look,"vehicle kept wheel/free look");CHECK(director.camera_proc==following_camera_update,"vehicle camera not restored");following=FALSE;unit.object.parent_object_index=NONE;director_choose_game_perspective(0,FALSE);director_choose_game_perspective(0,FALSE);CHECK(director.camera_proc==third_person_camera_update,"on-foot return");}
 else CASE("scripted-transition"){real_euler_angles2d f={0,0};struct unit_control_data c={0};on();alt=TRUE;third_person_camera_wanted(0);third_person_control(0,&f,&c);CHECK(third_person_frozen,"fixture did not freeze");director_set_camera(0,scripted_camera_update,FALSE);CHECK(!enabled&&!third_person_free_look&&!third_person_frozen,"scripted camera kept third-person input/frozen aim");CHECK(third_person_on,"camera preference lost");}
 else CASE("secondary-view"){on();director_set_camera(1,scripted_camera_update,FALSE);CHECK(enabled,"second view disabled first player's camera input");}
 else return 2;return 0;
}
