import json
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import screensaver

class DismissalTest(unittest.TestCase):
    def test_only_confirmed_screensaver_terminal_pids_are_signalled(self):
        clients = [{'class':'org.omarchy.screensaver','pid':7236},
                   {'class':'org.omarchy.agent','pid':7237},
                   {'class':'org.omarchy.screensaver','pid':True},
                   {'class':'org.omarchy.screensaver','pid':0},
                   None]
        with patch.object(screensaver.subprocess, 'run', return_value=SimpleNamespace(stdout=json.dumps(clients))) as run, patch.object(screensaver.os, 'kill') as kill:
            screensaver.close_saver_windows()
        run.assert_called_once()
        self.assertEqual(run.call_args.args[0], ['hyprctl','clients','-j'])
        kill.assert_called_once_with(7236, screensaver.signal.SIGTERM)

if __name__ == '__main__': unittest.main()
