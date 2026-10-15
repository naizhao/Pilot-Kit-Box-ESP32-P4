#!/usr/bin/env python3
"""用真实cache/loader组合行为测试逐项证伪两处地图退避缺陷。

所有回退只生成在临时目录；不修改生产、Git或设备。唯一替换目标先计数核验，
编译失败不算kill；必须是已编译的组合测试出现行为断言失败。
"""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
cache=(root/'firmware/main/pk_tile_cache.c').read_text()
loader=(root/'firmware/main/pk_tile_loader.c').read_text()
base=Path(tempfile.mkdtemp(prefix='pk_map_backoff_mutants_'))
env=os.environ.copy()
working_sdk=Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk')
if working_sdk.is_dir(): env.setdefault('SDKROOT',str(working_sdk))
cases=[
 ('resource_pollutes_sd','loader','    pk_tile_cache_bump_resource_fail(&s_cache, key, now_ms);','    pk_tile_cache_bump_sd_fail(&s_cache, key, now_ms);'),
 ('unsigned_future_ttl','cache','int32_t age = (int32_t)(now_ms - s->neg_timestamp_ms);','uint32_t age = now_ms - s->neg_timestamp_ms;'),
 ('no_modular_clock_wrap','cache','int32_t age = (int32_t)(now_ms - s->neg_timestamp_ms);','int32_t age = now_ms >= s->neg_timestamp_ms ? (int32_t)(now_ms - s->neg_timestamp_ms) : -1;'),
 ('resource_cannot_escalate','cache','s->sd_fail_count >= 5 || s->resource_fail_count >= 5','s->sd_fail_count >= 5'),
 ('put_keeps_resource_count','cache','    s->resource_fail_count = 0;\n}\n\nvoid pk_tile_cache_put_negative','}\n\nvoid pk_tile_cache_put_negative'),
 ('reset_keeps_resource_count','cache','    s->resource_fail_count = 0;\n    s->temp_negative','    s->temp_negative'),
 ('resource_bump_uses_sd_counter','cache','return bump_fail(cache, key, true);','return bump_fail(cache, key, false);'),
]
for name,unit,old,new in [('baseline',None,None,None)]+cases:
 d=base/name;d.mkdir();c=cache;l=loader
 if unit:
  s={'cache':c,'loader':l}[unit];assert s.count(old)==1,(name,s.count(old));s=s.replace(old,new)
  if unit=='cache':c=s
  else:l=s
 cp=d/'pk_tile_cache.c';lp=d/'pk_tile_loader.c';cp.write_text(c);lp.write_text(l)
 cmd=['/usr/bin/cc','-std=c11','-Wall','-Wextra','-O2','-I',str(root/'firmware/test/host_stubs'),'-I',str(root/'firmware/main'),f'-DPK_TEST_CACHE_SOURCE="{cp}"',f'-DPK_TEST_LOADER_SOURCE="{lp}"','-o',str(d/'test'),str(root/'firmware/test/test_pk_tile_backoff_integration.c')]
 built=subprocess.run(cmd,env=env,capture_output=True,text=True);(d/'build.log').write_text(built.stdout+built.stderr)
 assert built.returncode==0,(name,built.stderr)
 run=subprocess.run([str(d/'test')],capture_output=True,text=True);(d/'run.log').write_text(run.stdout+run.stderr)
 assert (run.returncode==0)==(name=='baseline'),(name,run.stdout)
 print(f'{name}: exit={run.returncode} {"PASS" if name=="baseline" else "KILLED"}')
print(f'artifacts: {base}')
