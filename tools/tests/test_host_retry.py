"""Group slot contention is a no-effect rejection, not an unknown outcome."""
import json
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'host/test'))
from motor_smoke import Device, retryable_rejection


class HostRetryTest(unittest.TestCase):
    def completed(self,code,result):
        return subprocess.CompletedProcess([],code,json.dumps(result),'')

    @mock.patch('motor_smoke.time.sleep')
    @mock.patch('motor_smoke.subprocess.run')
    def test_group_claim_contention_then_success(self,run,sleep):
        run.side_effect=[self.completed(5,{'ok':False,'result':5,'detail':14}),
                         self.completed(0,{'ok':True,'data':{'epoch':2}})]
        self.assertEqual(Device('cli','endpoint')('sub-enable','0','false'),{'epoch':2})
        self.assertEqual(run.call_count,2)
        sleep.assert_called_once_with(.02)

    @mock.patch('motor_smoke.subprocess.run')
    def test_timeout_is_an_unknown_outcome(self,run):
        run.return_value=self.completed(4,{'ok':False,'error':'timeout'})
        with self.assertRaises(AssertionError): Device('cli','endpoint')('sub-enable','0','false')
        self.assertEqual(run.call_count,1)

    @mock.patch('motor_smoke.subprocess.run')
    def test_opaque_action_busy_is_not_retried(self,run):
        run.return_value=self.completed(5,{'ok':False,'result':5,'detail':14})
        with self.assertRaises(AssertionError): Device('cli','endpoint')('action','114:0:770')
        self.assertEqual(run.call_count,1)

    @mock.patch('motor_smoke.subprocess.run')
    def test_expected_rejection_is_not_retried(self,run):
        rejection={'ok':False,'result':5,'detail':14}
        run.return_value=self.completed(5,rejection)
        self.assertEqual(Device('cli','endpoint')('sub-enable','0','false',code=5),rejection)
        self.assertEqual(run.call_count,1)

    def test_whitelist_and_withdrawal(self):
        for cmd in ('sub-config','sub-enable','sub-clear','cmd-config','cmd-clear'):
            self.assertTrue(retryable_rejection((cmd,),{'result':5,'detail':14}))
        for cmd in ('save','reboot','set','action'):
            self.assertFalse(retryable_rejection((cmd,),{'result':5,'detail':14}))
        self.assertFalse(retryable_rejection(('sub-enable',),{'result':9,'detail':14}))
        self.assertTrue(retryable_rejection(('set',),{'result':5,'detail':15}))
