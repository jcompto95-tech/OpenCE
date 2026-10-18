"""Local aiming and the action sent to a co-op peer must agree in free look."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import build,read,run,mutated,CHECK_FAILED,function

def generated(fault=None):
    code=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/third_person.c'),flags=re.M)
    has_filter='void third_person_action_facing(' in code
    if fault=='freeze':code=mutated(code,'*desired_facing = third_person_frozen_facing;',';')
    if fault=='head':code=mutated(code,'player_control_get_facing_direction(0, &control_data->looking_vector);','control_data->looking_vector = control_data->aiming_vector;')
    return (('under_test.inc','#define HAS_ACTION_FILTER '+str(int(has_filter))+'\n'+code),)

@pytest.mark.parametrize('case',['replicas','head','normal','second-player','release'])
def test_case(case):
    status,output=run(build('third_person_facing',generated()),case)
    assert status==0,output

@pytest.mark.parametrize('fault,case',[('freeze','replicas'),('head','head')])
def test_negative(fault,case):
    status,output=run(build('third_person_facing',generated(fault)),case)
    assert status==CHECK_FAILED,output

def test_input_hook():
    code=function(read('source/game/player_control.c'),'handle_one_player_input')
    assert code.index('action.desired_facing = player->desired_angles;')<code.index('third_person_action_facing(local_player_index, &action.desired_facing);')<code.index('update_client_queue(&action);')
