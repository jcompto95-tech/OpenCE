"""The gameplay third-person camera must not capture controller debug input."""
import sys
from pathlib import Path
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from harness import read,enum_with,structure,function,build,run,mutated,CHECK_FAILED

def generated(fault=False):
    source=read('source/camera/director.c');inputs=read('source/input/input.h')
    config='\n'.join(enum_with(inputs,n) for n in ['_key_escape','_gamepad_binary_button_right_thumb','_gamepad_stick_left'])
    config+='\n'+enum_with(source,'_camera_control_forward_bit')
    config+='\n'+enum_with(read('source/camera/director.h'),'_variable_height')
    config+='\n'+structure(source,'camera_control')
    code=function(source,'director_update_controls')
    if fault:code=mutated(code,' &&\n\t\t\tdirector->camera_proc != (director_camera_update_proc)third_person_camera_update','')
    return (('config.inc',config),('under_test.inc',code))

@pytest.mark.parametrize('case',['third-person','first-person','vehicle','debug-camera'])
def test_case(case):
    status,output=run(build('third_person_controller',generated()),case)
    assert status==0,output

def test_negative():
    status,output=run(build('third_person_controller',generated(True)),'third-person')
    assert status==CHECK_FAILED,output
