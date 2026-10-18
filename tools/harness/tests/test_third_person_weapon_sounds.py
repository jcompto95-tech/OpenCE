"""Local weapon animation sounds follow the active on-foot camera."""
import sys
from pathlib import Path
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import read, function, enum_with, build, run, mutated, CHECK_FAILED


def generated(fault=None):
    director = read('source/camera/director.c')
    code = function(director, 'director_get_perspective')
    code += '\n' + function(director, 'director_plays_first_person_weapon_sounds')
    update = function(read('source/interface/first_person_weapons.c'), 'first_person_weapon_update')
    start = update.index('\t\tif (sound_definition_index!=NONE &&')
    block = update[start:update.index('\n\t\tmoving=', start)]
    if fault == 'original':
        block = mutated(block, 'director_plays_first_person_weapon_sounds(local_player_index)',
                        'director_get_perspective(local_player_index)==_director_perspective_first_person')
    if fault == 'all-cameras':
        code = mutated(code, 'director_get(local_player_index)->camera_proc==third_person_camera_update', 'TRUE')
    code += '\nstatic void event(short local_player_index, long sound_definition_index)\n{\n'
    code += '\tstruct weapon_sound *first_person_weapon = &weapon;\n' + block + '\n}\n'
    return (('config.inc', enum_with(read('source/camera/director.h'), '_director_perspective_first_person')),
            ('under_test.inc', code))


@pytest.mark.parametrize('case', ['first', 'third', 'vehicle', 'death', 'scripted', 'debug',
                                 'no-event', 'transitions', 'secondary'])
def test_case(case):
    status, output = run(build('third_person_weapon_sounds', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('original', 'third'), ('all-cameras', 'vehicle')])
def test_negative(fault, case):
    status, output = run(build('third_person_weapon_sounds', generated(fault)), case)
    assert status == CHECK_FAILED, output
