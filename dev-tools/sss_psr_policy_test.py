"""SSS defaults retain unregularized transport; explicit blur requests survive."""
import argparse,json,sys
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--module',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();sys.path.insert(0,str(a.module.resolve()))
import pysuperluxcore as s
s.Init();rows=[]
cases=[('implicit-sss',True,{},0.),('explicit-auto-sss',True,{'path.regularization.auto':True},.03),
       ('authored-sigma-sss',True,{'path.regularization.sigma':.02},.02),
       ('auto-off-sss',True,{'path.regularization.auto':False},0.),
       ('implicit-glass',False,{},.03),('sigma-zero-glass',False,{'path.regularization.sigma':0.},0.),
       ('authored-sigma-glass',False,{'path.regularization.sigma':.025},.025),
       ('sigma-zero-sss-auto',True,{'path.regularization.auto':True,'path.regularization.sigma':0.},0.)]
for engine in ['PATHCPU','TILEPATHCPU','PATHOCL','TILEPATHOCL']:
 for name,sss,extra,expected in cases:
  scene=s.Scene();p=s.Properties();p.SetFromString('scene.camera.lookat.orig = 0 -3 0\nscene.camera.lookat.target = 0 0 0\nscene.lights.env.type = constantinfinite\nscene.materials.test.type = '+('openpbr\nscene.materials.test.subsurfaceweight = 1\nscene.materials.test.subsurfaceradius = .01 .01 .01\nscene.materials.test.subsurfacecolor = .8 .5 .2\n' if sss else 'glass\n'));scene.Parse(p)
  cfg=s.Properties();cfg.SetFromString('renderengine.type = '+engine+'\npath.lighttracing.auto = 0\npath.mnee.auto = 0\npath.photongi.caustic.auto = 0\n')
  for k,v in extra.items():cfg.Set(s.Property(k,v))
  config=s.RenderConfig(cfg,scene);effective=config.GetProperties();sigma=effective.Get('path.regularization.sigma').GetFloat() if effective.IsDefined('path.regularization.sigma') else 0.;half=effective.Get('path.regularization.halflife').GetFloat() if effective.IsDefined('path.regularization.halflife') else 0.
  row=dict(case=name,engine=engine,sigma=sigma,halflife=half,expected=expected);rows.append(row);print(json.dumps(row),flush=True)
  assert abs(sigma-expected)<1e-7,row
  if sss and name in ['implicit-sss','auto-off-sss','sigma-zero-sss-auto']:assert half==0,row
assert len(rows)==32
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(dict(passed=True,cases=rows),indent=2))
