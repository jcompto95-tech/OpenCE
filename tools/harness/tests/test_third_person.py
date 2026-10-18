"""Campaign camera eligibility, transitions and local-only free look."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, function, run, mutated, CHECK_FAILED


def generated(fault=None):
    code=re.sub(r'^#include "[^"\n]+"\n','',read('port/linux/game/third_person.c'),flags=re.M)
    code+='\n'+function(read('source/camera/director.c'),'director_set_camera')
    code+='\n'+function(read('source/camera/director.c'),'director_choose_game_perspective')
    if fault=='vehicle':
        code=mutated(code,'local_player_index == 0 && camera_proc !=', 'FALSE && camera_proc !=')
    if fault=='multiplayer':
        code=mutated(code,'!game_engine_running()','TRUE')
    return (('under_test.inc',code),)


@pytest.mark.parametrize('case',['toggle','eligibility','scope','distance','camera','free-look','vehicle-transition','scripted-transition','secondary-view'])
def test_case(case):
    status,output=run(build('third_person',generated()),case)
    assert status==0,output


@pytest.mark.parametrize('fault,case',[('vehicle','scripted-transition'),('multiplayer','eligibility')])
def test_negative(fault,case):
    status,output=run(build('third_person',generated(fault)),case)
    assert status==CHECK_FAILED,output
