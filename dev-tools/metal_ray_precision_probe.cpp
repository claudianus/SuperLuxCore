// Direct renderer device queries, not a replacement tracer or quality reference.
// Capture analytic double/Embree/GPU results and primary-derived continuations.
#include "luxrays/core/context.h"
#include "luxrays/core/device.h"
#include "luxrays/core/dataset.h"
#include "luxrays/core/trianglemesh.h"
#include "luxrays/core/hardwareintersectiondevice.h"
#include "luxrays/core/geometry/ray.h"
#include "luxrays/core/epsilon.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>
namespace slg { void (*SLG_DebugHandler)(const char *) = nullptr; }
using namespace luxrays;
using D3 = std::array<double,3>;
static D3 sub(D3 a,D3 b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
static D3 cross(D3 a,D3 b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
static double dot(D3 a,D3 b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static D3 d3(const Point &p) {return {p.x,p.y,p.z};}
struct Face {D3 p0,p1,p2;unsigned mesh,tri;};
struct Ref {bool hit=false;double t=INFINITY,b1=0,b2=0;unsigned mesh=0xffffffffu,tri=0xffffffffu;};
static Ref reference(const Ray &r,const std::vector<Face> &faces) {
    Ref result;D3 o={r.o.x,r.o.y,r.o.z},d={r.d.x,r.d.y,r.d.z};
    for(const auto &f:faces) {
        auto e1=sub(f.p1,f.p0),e2=sub(f.p2,f.p0),p=cross(d,e2);double det=dot(e1,p);
        if(std::abs(det)<1e-100)continue;
        auto tv=sub(o,f.p0);double u=dot(tv,p)/det;auto q=cross(tv,e1);
        double vv=dot(d,q)/det,t=dot(e2,q)/det;
        if(u < -1e-12 || vv < -1e-12 || u+vv>1+1e-12 || t<r.mint || t>r.maxt || t>=result.t)continue;
        result={true,t,u,vv,f.mesh,f.tri};
    }
    return result;
}
static void logger(const char *msg) {std::cerr<<msg<<'\n';}
struct Query {Ray ray;std::string group;bool intendedMiss=false;};
static Vector normalized(Vector v) {return v/std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
int main(int argc,char **argv) {
    if(argc!=3 && argc!=4) {std::cerr<<"usage: probe output.csv hwrt(0|1) [replay.csv]\n";return 2;}
    using Case=std::tuple<float,double,float>;
    std::map<Case,std::vector<Query>> replay;
    if(argc==4) {
        std::ifstream input(argv[3]);if(!input)throw std::runtime_error("Cannot open replay input");
        std::string line;std::getline(input,line);
        while(std::getline(input,line)) {
            std::istringstream row(line);std::vector<std::string> f;std::string value;
            while(std::getline(row,value,','))f.push_back(value);
            if(f.size()!=28)throw std::runtime_error("Invalid replay CSV row");
            Ray ray(Point(std::stof(f[5]),std::stof(f[6]),std::stof(f[7])),
                    Vector(std::stof(f[8]),std::stof(f[9]),std::stof(f[10])),
                    std::stof(f[11]),std::stof(f[12]),.5f);
            replay[{std::stof(f[0]),std::stod(f[1]),std::stof(f[2])}].push_back(
                    {ray,f[3],f[13]=="1"});
        }
        if(replay.size()!=6)throw std::runtime_error("Replay must contain all six geometry cases");
    }
    setenv("LUXRAYS_METAL_HWRT",argv[2],1);
    std::ofstream out(argv[1]);if(!out) return 2;
    std::ofstream geometry(std::string(argv[1])+".geometry.csv");if(!geometry)return 2;
    geometry<<std::setprecision(17)<<"extent,tilt,gap,mesh,vertex,x,y,z\n";
    out<<std::setprecision(17);
    out<<"extent,tilt,gap,group,index,ox,oy,oz,dx,dy,dz,mint,maxt,intended_miss,ref_hit,ref_t,ref_mesh,ref_tri,gpu_hit,gpu_t,gpu_b1,gpu_b2,gpu_mesh,gpu_tri,cpu_hit,cpu_t,cpu_mesh,cpu_tri\n";
    size_t total=0,unexpected=0;
    for(float extent:{100.f,10000.f})for(double angle:{0.,17.})for(float gap:{0.f,.0001f}) {
        // At large rotated coordinates a.0001 offset may round away in
        // input vertices. Keep the thin-gap oracle on exactly represented
        // horizontal planes; tilted planes still exercise origin precision.
        if(angle!=0 && gap>0)continue;
        // Meshes outlive Context/DataSet, including exception unwinding.
        std::vector<std::unique_ptr<TriangleMesh>> meshes;
        Context ctx(logger);DeviceDescriptions chosen;
        for(auto d:ctx.GetAvailableDeviceDescriptions())if(d.get().GetType()&DEVICE_TYPE_METAL_ALL){chosen.push_back(d);break;}
        if(chosen.empty())throw std::runtime_error("No Metal device");
        auto devices=ctx.AddIntersectionDevices(chosen);auto ds=std::make_shared<DataSet>(ctx);
        const double rad=angle*3.14159265358979323846/180.;const float co=std::cos(rad),si=std::sin(rad);
        Vector n(0,-si,co),u(1,0,0),v(0,co,si);std::vector<Face> faces;
        for(unsigned mesh=0;mesh<(gap>0?2u:1u);++mesh) {
            VertexBuffer vertices(4);float z=mesh?gap:0;
            vertices[0]=Point(-extent,-extent*co-z*si,-extent*si+z*co);
            vertices[1]=Point(extent,-extent*co-z*si,-extent*si+z*co);
            vertices[2]=Point(extent,extent*co-z*si,extent*si+z*co);
            vertices[3]=Point(-extent,extent*co-z*si,extent*si+z*co);
            for(unsigned k=0;k<4;++k)geometry<<extent<<','<<angle<<','<<gap<<','<<mesh<<','<<k<<','<<vertices[k].x<<','<<vertices[k].y<<','<<vertices[k].z<<'\n';
            TriangleBuffer triangles(2);triangles[0]=Triangle(0,1,2);triangles[1]=Triangle(0,2,3);
            faces.push_back({d3(vertices[0]),d3(vertices[1]),d3(vertices[2]),mesh,0});
            faces.push_back({d3(vertices[0]),d3(vertices[2]),d3(vertices[3]),mesh,1});
            meshes.push_back(std::make_unique<TriangleMesh>(std::move(vertices),std::move(triangles)));ds->Add(*meshes.back());
        }
        ds->SetAcceleratorType(ACCEL_AUTO);ds->Preprocess();ctx.SetDataSet(ds);ctx.Start();
        auto &dev=dynamic_cast<HardwareIntersectionDevice&>(devices[0].get());
        auto cpu=ds->GetAccelerator(ACCEL_EMBREE);
        std::vector<Query> queries;
        auto add=[&](Point o,Vector d,const std::string &group,bool miss){Ray r(o,normalized(d),0.f,1000.f,.5f);queries.push_back({r,group,miss});};
        for(int y=0;y<12;++y)for(int x=0;x<12;++x) {
            Point p=Point(0,0,0)+u*((x+.37f)/6-1)+v*((y+.19f)/6-1);Point o(0,-5,1.5f);add(o,p-o,"primary",false);
        }
        for(int y=0;y<8;++y)for(int x=0;x<8;++x)for(int j=0;j<8;++j) {
            Point q=Point(0,0,0)+u*((x+.31f)/4-1)+v*((y+.13f)/4-1);
            const float eps=MachineEpsilon::E(q);Point o=q+n*eps;
            const float z=(j+.5f)/8,phi=(j*2.39996323f+x*.71f+y*.29f),s=std::sqrt(1-z*z);
            add(o,u*(s*std::cos(phi))+v*(s*std::sin(phi))+n*z,"escape",gap==0);
        }
        auto trace=[&](size_t begin,size_t end,std::vector<RayHit>&hits) {
            std::vector<Ray> rays;for(size_t i=begin;i<end;++i)rays.push_back(queries[i].ray);hits.resize(rays.size());
            for(auto &h:hits){h.SetMiss();h.t=INFINITY;h.b1=h.b2=0;h.triangleIndex=0xffffffffu;}
            HardwareDeviceBuffer *rb=nullptr,*hb=nullptr;
            try {dev.AllocBufferRW(&rb,rays.data(),rays.size()*sizeof(Ray),"precision rays");dev.AllocBufferRW(&hb,hits.data(),hits.size()*sizeof(RayHit),"precision hits");dev.EnqueueTraceRayBuffer(rb,hb,rays.size());dev.FinishQueue();dev.EnqueueReadBuffer(hb,true,hits.size()*sizeof(RayHit),hits.data());dev.FinishQueue();dev.FreeBuffer(&rb);dev.FreeBuffer(&hb);}
            catch(...){if(rb)dev.FreeBuffer(&rb);if(hb)dev.FreeBuffer(&hb);throw;}
        };
        if(argc==4)queries=replay.at({extent,angle,gap});
        const size_t first=queries.size();std::vector<RayHit> hits;trace(0,first,hits);
        for(size_t i=0;argc==3 && i<144;++i)if(!hits[i].Miss()) {
            const auto &r=queries[i].ray;const float t=hits[i].t;
            Point q(std::fma(t,r.d.x,r.o.x),std::fma(t,r.d.y,r.o.y),std::fma(t,r.d.z,r.o.z));
            Point o=q+n*MachineEpsilon::E(q);
            for(int j=0;j<8;++j) {float z=(j+.5f)/8,phi=j*2.39996323f,s=std::sqrt(1-z*z);add(o,u*(s*std::cos(phi))+v*(s*std::sin(phi))+n*z,"primary_derived_escape",true);}
        }
        if(queries.size()>first) {
            std::vector<RayHit> tails;trace(first,queries.size(),tails);hits.insert(hits.end(),tails.begin(),tails.end());
        }
        for(size_t i=0;i<queries.size();++i) {
            const auto &q=queries[i];const auto &a=q.ray;const auto &g=hits[i];auto ref=reference(a,faces);Ray cr=a;RayHit ch;ch.SetMiss();ch.t=INFINITY;ch.triangleIndex=0xffffffffu;cpu->Intersect(&cr,&ch);
            bool gh=!g.Miss();if(gh!=ref.hit || (gh&&g.meshIndex!=ref.mesh))++unexpected;
            out<<extent<<','<<angle<<','<<gap<<','<<q.group<<','<<i<<','<<a.o.x<<','<<a.o.y<<','<<a.o.z<<','<<a.d.x<<','<<a.d.y<<','<<a.d.z<<','<<a.mint<<','<<a.maxt<<','<<q.intendedMiss<<','<<ref.hit<<','<<ref.t<<','<<ref.mesh<<','<<ref.tri<<','<<gh<<','<<g.t<<','<<g.b1<<','<<g.b2<<','<<g.meshIndex<<','<<g.triangleIndex<<','<<!ch.Miss()<<','<<ch.t<<','<<ch.meshIndex<<','<<ch.triangleIndex<<'\n';
        }
        total+=queries.size();ctx.Stop();std::cout<<"captured extent="<<extent<<" tilt="<<angle<<" gap="<<gap<<" rays="<<queries.size()<<std::endl;
    }
    std::cout<<"capture_complete rays="<<total<<" gpu_hit_or_mesh_mismatch="<<unexpected<<"; diagnostic capture, not quality pass\n";
}
