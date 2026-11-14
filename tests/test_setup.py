import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
loader=importlib.machinery.SourceFileLoader('setup',str(ROOT/'bin/setup'))
spec=importlib.util.spec_from_loader('setup',loader)
setup=importlib.util.module_from_spec(spec);loader.exec_module(setup)

class SetupTests(unittest.TestCase):
    def setUp(self):
        self.enterContext(patch.dict("os.environ", {"OMARCHY_SETUP_LAUNCHER":str(ROOT/"tests/fixtures/omarchy-launch-screensaver")}))
        self.temp=tempfile.TemporaryDirectory(prefix='setup-test-')
        self.addCleanup(self.temp.cleanup)
        self.home=Path(self.temp.name)/'different user';self.home.mkdir()
    def plan(self):return setup.file_plan(self.home,False,False)
    def test_plan_has_no_home_side_effects(self):
        writes=self.plan();self.assertTrue(writes);self.assertEqual(list(self.home.iterdir()),[])
        self.assertNotIn('.config/hypr/monitors.lua',writes)
        self.assertNotIn('.codex/auth.json',writes)
        self.assertFalse(any('kiat.agents' in x for x in writes))
    def test_apply_and_rollback(self):
        path=self.home/'.config/omarchy/shell.json';path.parent.mkdir(parents=True)
        original={'bar':{'layout':{'center':[{'id':'omarchy.clock','format':'HH:mm'}]}},'idle':{'lock':420},'custom':'keep'}
        path.write_text(json.dumps(original));before=path.read_bytes()
        backup=setup.apply(self.home,self.plan())
        config=json.loads(path.read_text())
        self.assertEqual(config['idle']['lock'],420);self.assertEqual(config['custom'],'keep')
        self.assertEqual(config['bar']['layout']['center'][0]['format'],'HH:mm')
        self.assertEqual(config['bar']['layout']['center'][0]['id'],'kiat.clock')
        self.assertEqual(backup.stat().st_mode&0o777,0o700)
        setup.restore(self.home,backup,self.plan())
        self.assertEqual(path.read_bytes(),before)
        self.assertFalse((self.home/'.config/omarchy/plugins/kiat.vigil/BarWidget.qml').exists())
    def test_launcher_uses_target_home_and_safe_quotes(self):
        launch=self.plan()['.config/omarchy/ambient/launch'][0].decode()
        self.assertIn(str(self.home),launch)
        self.assertNotIn('/home/original-user/',launch)
        self.assertIn("'"+str(self.home/' .config').replace(' .config','.config'),launch)
    def test_idempotent_layout(self):
        c=setup.layout(json.loads((ROOT/'config/shell-default.json').read_text()),True,True)
        self.assertEqual(setup.layout(c,True,True),c)
    def test_symlink_cannot_write_outside_home(self):
        outside=Path(self.temp.name)/'outside';outside.mkdir()
        (self.home/'.config').symlink_to(outside,target_is_directory=True)
        with self.assertRaises(ValueError):self.plan()
        self.assertEqual(list(outside.iterdir()),[])
    def test_backup_tampering_is_rejected(self):
        setup.apply(self.home,self.plan());backup=setup.backup(self.home,self.plan())
        f=backup/'files/.config/omarchy/shell.json';f.write_text('tampered')
        with self.assertRaises(ValueError):setup.restore(self.home,backup,self.plan())
    def test_cross_machine_restore_rebases_home(self):
        setup.apply(self.home,self.plan());backup=setup.backup(self.home,self.plan())
        other=Path(self.temp.name)/'new-user';other.mkdir()
        setup.restore(other,backup,setup.file_plan(other))
        text=(other/'.config/omarchy/ambient/launch').read_text()
        self.assertIn(str(other),text);self.assertNotIn(str(self.home),text)
    def test_restore_rejects_traversal(self):
        backup=setup.backup(self.home,self.plan());p=backup/'manifest.json'
        data=json.loads(p.read_text());data['files']['../outside']={'existed':False};p.write_text(json.dumps(data))
        with self.assertRaises(ValueError):setup.restore(self.home,backup,self.plan())
    def test_cli_restores_corrupt_config(self):
        backup=setup.apply(self.home,self.plan())
        (self.home/'.config/omarchy/shell.json').write_text('bad json')
        p=subprocess.run([str(ROOT/'bin/setup'),'restore','--home',str(self.home),'--from',str(backup)],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertFalse((self.home/'.config/omarchy/shell.json').exists())

if __name__=='__main__':unittest.main()
