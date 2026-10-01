import signal, sys, tempfile, threading, unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import manager

class ManagerTests(unittest.TestCase):
 def setUp(self):
  self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
  self.model=manager.Manager.__new__(manager.Manager)
  self.model.child=None;self.model.lock=threading.Lock();self.model.stopping=set()
  self.model.log=Path(self.temporary.name)/'launch.log';self.model.ini=Path(self.temporary.name)/'bridge.ini'
  self.model.ini.write_text('[Bridge]\n; preserve this\nNpcDamageScale=0.03\nCarModel=rocketbridge\\fennec.nif\n')
 def test_settings_validation_and_preservation(self):
  with patch.object(self.model,'instances',return_value=[]),patch.object(manager,'ROOT',Path(self.temporary.name)):
   data=self.model.settings();data['NpcDamageScale']=.05;self.model.save(data)
   self.assertEqual(self.model.settings()['NpcDamageScale'],.05)
   self.assertIn('; preserve this',self.model.ini.read_text());self.assertIn('CarModel=rocketbridge\\fennec.nif',self.model.ini.read_text())
   self.assertEqual(len(list((Path(self.temporary.name)/'build/ui-settings-backups').glob('*'))),1)
   data['NpcDamageScale']=float('nan')
   with self.assertRaises(ValueError):self.model.save(data)
 def test_active_session_blocks_start_and_save(self):
  with patch.object(self.model,'instances',return_value=[{'active':True}]):
   with self.assertRaises(ValueError):self.model.start({'mode':'desktop'})
   with self.assertRaises(ValueError):self.model.save(self.model.settings())
 def test_stop_only_verified_launcher(self):
  with patch.object(manager,'command',return_value=None),patch.object(manager.os,'kill') as kill:
   with self.assertRaises(ValueError):self.model.stop({'pid':123})
   kill.assert_not_called()
  with patch.object(manager,'command',return_value=['python','tools/launch.py']),patch.object(manager.os,'kill') as kill:
   self.model.stop({'pid':123});kill.assert_called_once_with(123,signal.SIGINT)
 def test_unknown_mode_cannot_launch(self):
  with patch.object(self.model,'instances',return_value=[]),patch.object(manager.subprocess,'Popen') as spawn:
   with self.assertRaises(ValueError):self.model.start({'mode':'bogus'})
   spawn.assert_not_called()

 def test_independent_displays(self):
  from types import SimpleNamespace
  self.model.log=Path(self.temporary.name)/'launch.log'
  for rl_display in ('private','desktop'):
   for sky_display in ('private','desktop'):
    games={'rl':{'enabled':True,'display':rl_display,'workspace':4,'monitor':''},'skyrim':{'enabled':True,'display':sky_display,'workspace':3,'monitor':''}}
    with patch.object(self.model,'instances',return_value=[]),patch.object(manager.subprocess,'Popen',return_value=SimpleNamespace(pid=123)) as spawn:
     self.model.start({'games':games});args=spawn.call_args.args[0]
     self.assertEqual('--desktop-rl' in args,rl_display=='desktop')
     self.assertEqual('--desktop-skyrim' in args,sky_display=='desktop')
 def test_skyrim_only(self):
  from types import SimpleNamespace
  self.model.log=Path(self.temporary.name)/'launch.log'
  games={'rl':{'enabled':False,'display':'private','workspace':4,'monitor':''},'skyrim':{'enabled':True,'display':'desktop','workspace':7,'monitor':''}}
  with patch.object(self.model,'instances',return_value=[]),patch.object(manager.subprocess,'Popen',return_value=SimpleNamespace(pid=123)) as spawn:
   self.model.start({'games':games});args=spawn.call_args.args[0]
   self.assertIn('--no-rl',args);self.assertEqual(args[args.index('--skyrim-workspace')+1],'7')
