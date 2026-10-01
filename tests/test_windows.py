import json,struct,sys,tempfile,threading,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock,patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import windows_platform as platform
import windows_manager,launch_windows,install_windows,install,prepare_bullet

class WindowsTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
 def model(self):
  model=windows_manager.WindowsManager.__new__(windows_manager.WindowsManager)
  model.child=None;model.lock=threading.Lock();model.stopping=set();model.log=self.root/'launch.log'
  model.ini=self.root/'bridge.ini';model.ini.write_text('[Bridge]\n')
  return model
 def test_separate_steam_libraries(self):
  steam=self.root/'Steam';library=self.root/'Second library';(steam/'steamapps').mkdir(parents=True)
  (library/'steamapps/common/Skyrim Special Edition').mkdir(parents=True)
  (steam/'steamapps/libraryfolders.vdf').write_text('"libraryfolders" { "1" { "path" "'+str(library)+'" } }')
  (library/'steamapps/appmanifest_489830.acf').write_text('"AppState" { "installdir" "Skyrim Special Edition" "buildid" "123" }')
  game,build=platform.find_game(steam,489830)
  self.assertEqual(game,library/'steamapps/common/Skyrim Special Edition');self.assertEqual(build,'123')
  (library/'steamapps/appmanifest_489830.acf').write_text('"installdir" "../escape"')
  with self.assertRaises(ValueError):platform.find_game(steam,489830)
 def test_native_paths_preserve_backslashes(self):
  original='[Bridge]\n; retained\nTerrainMeshPath=Z:\\old\\mesh.rltm\nNpcDamageScale=.05\n'
  path=r'C:\Users\Me\AppData\Roaming\bakkesmod\bakkesmod\data\rocket-skyrim-terrain.rltm'
  text=install_windows.ini_settings(original,{'TerrainMeshPath':path,'NativeTerrain':1})
  self.assertIn('TerrainMeshPath='+path,text);self.assertIn('; retained',text);self.assertIn('NpcDamageScale=.05',text)
 def test_missing_assets_prevents_install(self):
  with self.assertRaisesRegex(ValueError,'Missing local asset'):install_windows.asset_files(self.root)
 def test_missing_material_map_prevents_grey_visuals(self):
  files=['SKSE/Plugins/RocketBridge-Car.rmesh','SKSE/Plugins/RocketBridge-Ball.rmesh','meshes/rocketbridge/fennec.nif','meshes/rocketbridge/ball.nif','textures/rocketbridge/boost_flame.dds','textures/rocketbridge/flat_n.dds']
  for name in files:
   path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'asset')
  texture=b'textures/rocketbridge/test_d.dds'
  mesh=struct.pack('<III',0x314D4252,1,len(texture))+texture+struct.pack('<I',3)+bytes(3*48)
  for name in files[:2]:(self.root/name).write_bytes(mesh)
  with self.assertRaisesRegex(ValueError,'Missing material map'):install_windows.asset_files(self.root)
  (self.root/'textures/rocketbridge/test_d.dds').write_bytes(b'diffuse')
  (self.root/'textures/rocketbridge/test_n.dds').write_bytes(b'normal')
  self.assertEqual(len(install_windows.asset_files(self.root)),8)
 def test_installer_atomic_and_preserves_backup(self):
  with patch.object(install,'ROOT',self.root):
   target=self.root/'game/plugin.dll';target.parent.mkdir();target.write_bytes(b'old')
   installer=install.Installer();installer.put(target,b'new')
   self.assertEqual(target.read_bytes(),b'new');self.assertEqual(Path(installer.entries[0]['backup']).read_bytes(),b'old')
   installer.put(target,b'new');self.assertEqual(len(installer.entries),1)
   # A locked destination must preserve the old DLL, not truncate it.
   with patch.object(install.os,'replace',side_effect=PermissionError('locked')):
    with self.assertRaises(PermissionError):installer.put(target,b'broken')
   self.assertEqual(target.read_bytes(),b'new')
 def test_stop_never_touches_attached_games(self):
  process=Mock()
  with patch.object(launch_windows,'psutil_module',return_value=SimpleNamespace(Error=Exception)),patch.object(launch_windows,'matching_process',return_value=process) as matching:
   launch_windows.stop_owned([{'owned':False},{'owned':True}])
   matching.assert_called_once_with({'owned':True});process.terminate.assert_called_once()
 def test_reused_pid_not_owned(self):
  process=Mock();process.create_time.return_value=20;process.exe.return_value='RocketLeague.exe'
  api=SimpleNamespace(Process=Mock(return_value=process),Error=RuntimeError)
  with patch.object(platform,'psutil_module',return_value=api):
   self.assertIsNone(platform.matching_process(dict(pid=1,created=10,exe='RocketLeague.exe')))
   self.assertIs(platform.matching_process(dict(pid=1,created=20,exe='RocketLeague.exe')),process)
   self.assertIsNone(platform.matching_process(dict(pid=1,created=20,exe='Other.exe')))
 def test_launch_uses_steam_and_skse(self):
  choices={g:dict(enabled=True,display='desktop') for g in ('rl','skyrim')}
  commands=launch_windows.plan(dict(steam='Steam',rl='RL',skyrim='Skyrim'),choices)
  self.assertEqual(commands['rl'][1:],['-applaunch','252950','-NoEAC']);self.assertTrue(commands['skyrim'][0].endswith('skse64_loader.exe'))
  self.assertFalse(any('proton' in value.lower() for command in commands.values() for value in command))
 def test_ui_native_display_choices_and_spawn(self):
  model=self.model()
  choices={g:dict(enabled=True,display='desktop',monitor='') for g in ('rl','skyrim')};choices['rl']['display']='minimized'
  with patch.object(windows_manager,'ROOT',self.root),patch.object(model,'instances',return_value=[]),patch.object(model,'monitors',return_value=[]),patch.object(windows_manager.subprocess,'Popen',return_value=SimpleNamespace(pid=123)) as spawn:
   self.assertEqual(model.start({'games':choices})['pid'],123)
   args=spawn.call_args.args[0];self.assertTrue(args[1].endswith('launch_windows.py'));self.assertEqual(spawn.call_args.kwargs['creationflags'],0x08000000)
   stored=json.loads(Path(args[-1]).read_text());self.assertEqual(stored['rl']['display'],'minimized')
   choices['rl']['display']='private'
   with self.assertRaises(ValueError):model.start({'games':choices})
 def test_stop_uses_request_file_and_verifies_creation(self):
  folder=self.root/'build/sessions/win-1';folder.mkdir(parents=True);(folder/'session.json').write_text(json.dumps(dict(pid=123,created=10)))
  model=self.model();api=SimpleNamespace(Process=Mock(return_value=SimpleNamespace(create_time=lambda:10)),Error=RuntimeError)
  with patch.object(windows_manager,'ROOT',self.root),patch.object(windows_manager,'script_command',return_value=['python','launch_windows.py']),patch.object(windows_manager,'psutil_module',return_value=api):
   model.stop({'pid':123});self.assertTrue((folder/'stop.request').exists())
   (folder/'stop.request').unlink();api.Process.return_value=SimpleNamespace(create_time=lambda:11)
   with self.assertRaises(ValueError):model.stop({'pid':123})
   self.assertFalse((folder/'stop.request').exists())
 def test_uniform_bullet_patch_idempotent(self):
  for name in prepare_bullet.PATCHED:
   path=self.root/'original/src'/name;path.parent.mkdir(parents=True,exist_ok=True)
   text='vector , 0x80)' if not name.endswith('btCollisionShape.h') else 'virtual void getAabb(const btTransform& t,btVector3& aabbMin,btVector3& aabbMax) const =0;'
   path.write_text(text)
  dest=prepare_bullet.prepare(self.root/'original',self.root/'private')
  shape=dest/'src'/prepare_bullet.PATCHED[-1];stamp=shape.stat().st_mtime_ns
  self.assertEqual(shape.read_text().count('virtual void getAabbSlow'),1)
  prepare_bullet.prepare(self.root/'original',dest);self.assertEqual(shape.stat().st_mtime_ns,stamp)

if __name__=='__main__':unittest.main()
