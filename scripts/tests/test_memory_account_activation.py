#!/usr/bin/env python3
"""Run as root only in a disposable validation container."""
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import uuid

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('activation',ROOT/'deploy/container/memory-account-activate.py')
activation=importlib.util.module_from_spec(spec)
spec.loader.exec_module(activation)


@unittest.skipUnless(os.geteuid()==0,'requires disposable root container')
class AccountActivation(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.home=Path(self.temp.name)
        (self.home/'private-memory').mkdir()
        self.roster=self.home/'private-memory/pam-accounts.json'
        self.node=str(uuid.uuid4())
        self.configuration=self.home/'database.json'
        self.configuration.write_text(json.dumps(dict(pam_account_registry=dict(path=str(self.roster),user_template='aimee',maintenance_template='native-maintenance:aimee'),host_subject_grants={'aimee':{'access':{'local_node':self.node,'principal':{'kind':'pam'}}}})))
        self.pwd=mock.patch.object(activation.pwd,'getpwnam',side_effect=lambda name:SimpleNamespace(pw_uid=0 if name=='root' else 1000,pw_gid=1000))
        self.grp=mock.patch.object(activation.grp,'getgrnam',return_value=SimpleNamespace(gr_gid=1000,gr_mem=['alice','bob']))
        self.pwd.start();self.grp.start()

    def tearDown(self):
        self.pwd.stop();self.grp.stop();self.temp.cleanup()

    def activate(self,name):
        activation.activate(name,self.home,self.configuration)

    def test_activation_is_atomic_concurrent_idempotent_and_root_owned(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(self.activate,['alice','bob']*8))
        self.assertEqual(json.loads(self.roster.read_text()),dict(schema_version=1,node=self.node,accounts=['alice','bob']))
        meta=self.roster.stat()
        self.assertEqual(meta.st_uid,0)
        self.assertEqual(meta.st_mode & 0o777,0o640)
        self.assertEqual(list(self.roster.parent.glob('.pam-accounts-*')),[])

    def test_names_root_accounts_and_request_paths_cannot_activate(self):
        for name in ['root','../alice','native-maintenance:alice','alice;id','Alice']:
            with self.assertRaises(ValueError):self.activate(name)
        self.assertFalse(self.roster.exists())
        with mock.patch.object(activation.grp,'getgrnam',return_value=SimpleNamespace(gr_gid=2000,gr_mem=[])):
            with self.assertRaises(ValueError):self.activate('alice')
        self.assertFalse(self.roster.exists())

    def test_symlink_and_untrusted_modes_are_refused_without_replacing_them(self):
        target=self.home/'target';target.write_text('preserve')
        self.roster.symlink_to(target)
        with self.assertRaises(OSError):self.activate('alice')
        self.assertEqual(target.read_text(),'preserve')
        self.roster.unlink();self.activate('alice')
        raw=self.roster.read_bytes();self.roster.chmod(0o666)
        with self.assertRaises(ValueError):self.activate('bob')
        self.assertEqual(self.roster.read_bytes(),raw)


if __name__=='__main__':unittest.main()
