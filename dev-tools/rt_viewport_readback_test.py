"""Execute production demand-read methods with controlled producer completion.

ASan/UBSan cover cancelled buffer access, running-copy lease retention,
bounded admission, stop wakeup and cancelled-request retirement.
"""
from pathlib import Path
import subprocess,tempfile,argparse,json,hashlib
root=Path(__file__).resolve().parents[1];a=argparse.ArgumentParser();a.add_argument('--out',type=Path,required=True);args=a.parse_args()
s=(root/'src/slg/engines/rtpathocl/rtpathocl.cpp').read_text();header=(root/'include/slg/engines/rtpathocl/rtpathocl.h').read_text();start=header.index('\tstruct ViewportReadRequest {');last=header.index('\n',header.index('std::atomic<bool> viewportReadbackStopped',start));fields=header[start:last]
methods=[]
for name in ['ReadViewportOutput','ServiceViewportReadbacks','CancelViewportReadbacks']:
 start=s.index(('PropertiesUPtr ' if name=='ReadViewportOutput' else 'void ')+'RTPathOCLRenderEngine::'+name);begin=s.index('{',start);end=begin+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 methods.append(s[start:end].replace('RTPathOCLRenderEngine::','Engine::'))
code=r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
using namespace std;
namespace luxrays { using PropertiesUPtr=unique_ptr<int>; }
using luxrays::PropertiesUPtr;
struct Film{atomic<int>eye{0};double GetTotalEyeSampleCount(){return eye.load();}};
struct Engine{Film film;mutex filmGuard;mutex*filmMutex=&filmGuard;
 Film&GetFilm(){return film;}
 Engine(){viewportReadbackStopped=false;}
 PropertiesUPtr ReadViewportOutput(function<PropertiesUPtr(Film&)>read);
 void ServiceViewportReadbacks();void CancelViewportReadbacks();
FIELDS
 bool queued(){lock_guard<mutex>g(viewportReadsMutex);return !viewportReads.empty();}
 size_t size(){lock_guard<mutex>g(viewportReadsMutex);return viewportReads.size();}
};
METHODS
void waitQueued(Engine&e){for(int i=0;i<200&&!e.queued();++i)this_thread::sleep_for(chrono::milliseconds(1));if(!e.queued())throw 1;}
int main(){int errors=0;auto check=[&](const char*n,bool ok){printf("%s %s\n",ok?"PASS":"FAIL",n);errors+=!ok;};
 {Engine e;atomic<int>writes{0};int*buffer=new int(7);auto r=e.ReadViewportOutput([&](Film&){*buffer=9;++writes;return make_unique<int>(3);});delete buffer;e.ServiceViewportReadbacks();check("timeout_cancels_before_buffer_lease_ends",!r&&writes==0);}
 {Engine e;atomic<bool>done{false};thread caller([&]{auto r=e.ReadViewportOutput([](Film&){return make_unique<int>(3);});done=true;});waitQueued(e);e.viewportReadbackStopped=true;e.CancelViewportReadbacks();caller.join();check("stop_wakes_queued_reader",done);}
 {Engine e;atomic<bool>entered{false},release{false},done{false};thread caller([&]{auto r=e.ReadViewportOutput([&](Film&){entered=true;while(!release)this_thread::yield();return make_unique<int>(17);});if(!r||*r!=17)++errors;done=true;});waitQueued(e);thread producer([&]{e.ServiceViewportReadbacks();});while(!entered)this_thread::yield();this_thread::sleep_for(chrono::milliseconds(300));check("running_copy_keeps_lease_past_wait_timeout",!done);release=true;producer.join();caller.join();check("running_copy_returns_owned_result",done);}
 {Engine e;vector<thread>callers;for(int i=0;i<8;++i)callers.emplace_back([&]{e.ReadViewportOutput([](Film&){return make_unique<int>(1);});});this_thread::sleep_for(chrono::milliseconds(40));check("request_queue_bounded_four",e.size()<=4);e.viewportReadbackStopped=true;e.CancelViewportReadbacks();for(auto&t:callers)t.join();}
 {Engine e;for(int i=0;i<3;++i)e.ReadViewportOutput([](Film&){return make_unique<int>(1);});e.ServiceViewportReadbacks();check("cancelled_request_mutex_lifetime_retired",e.size()==0);}
 {Engine e;e.film.eye=1;int calls=0;auto r=e.ReadViewportOutput([&](Film&){++calls;return make_unique<int>(8);});check("ready_film_read_immediate",r&&*r==8&&calls==1&&e.size()==0);}
 return errors?1:0;}
'''.replace('FIELDS',fields).replace('METHODS','\n'.join(methods))
with tempfile.TemporaryDirectory(prefix='slc-demand-read-') as d:
 f=Path(d)/'test.cpp';exe=Path(d)/'test';f.write_text(code);c=subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(f),'-o',str(exe)],text=True,capture_output=True,timeout=30);assert c.returncode==0,c.stderr;r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=10)
report={'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'scope':__doc__};args.out.write_text(json.dumps(report,indent=2)+'\n');print(r.stdout);assert r.returncode==0,report
