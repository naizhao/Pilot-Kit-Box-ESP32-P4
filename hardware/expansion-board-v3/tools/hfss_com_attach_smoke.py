#!/usr/bin/env python3
"""附着指定的桌面 AEDT Student 进程，验证 Windows COM 自动化。"""

from __future__ import annotations

import json
import os
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path


WORK_DIR = Path.home() / "Documents" / "HFSS_IFA_Codex"
STATUS_FILE = WORK_DIR / "hfss_com_attach_status.json"


def write_status(**values: object) -> None:
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    payload = {
        "updated_at": datetime.now(timezone.utc).isoformat(),
        **values,
    }
    STATUS_FILE.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )


def main() -> int:
    target_pid = int(sys.argv[1])
    os.environ["ANSYSEMSV_ROOT252"] = (
        r"C:\Program Files\ANSYS Inc\ANSYS Student\v252\AnsysEM"
    )
    write_status(state="starting", target_pid=target_pid)
    desktop = None
    try:
        from ansys.aedt.core import Desktop
        from ansys.aedt.core.generic.settings import settings

        settings.use_grpc_api = False
        desktop = Desktop(
            version="2025.2",
            non_graphical=False,
            new_desktop=False,
            close_on_exit=False,
            student_version=True,
            aedt_process_id=target_pid,
        )
        write_status(
            state="ok",
            target_pid=target_pid,
            connected_pid=desktop.aedt_process_id,
            aedt_version=desktop.aedt_version_id,
            projects=list(desktop.project_list),
        )
        return 0
    except Exception as exc:
        write_status(
            state="error",
            target_pid=target_pid,
            error=repr(exc),
            traceback=traceback.format_exc(),
        )
        return 1
    finally:
        if desktop is not None:
            desktop.release_desktop(close_projects=False, close_desktop=False)


if __name__ == "__main__":
    raise SystemExit(main())
