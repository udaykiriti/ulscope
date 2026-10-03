#include <QApplication>
#include <QPixmap>
#include <QElapsedTimer>
#include <cstdio>
#include "ui/ulpplot.h"
using namespace ulpscope;
int main(int argc,char**argv){
  QApplication app(argc,argv);
  for (uint64_t n : {65536ull, 1ull<<20}) {
    ScanResult r; r.config.fmt=FloatFormat::bf16(); r.config.symbol="x";
    r.samples.resize(n);
    uint32_t s=1;
    for(uint64_t i=0;i<n;i++){ s=s*1664525u+1013904223u;
      r.samples[i].inputBits=s; r.samples[i].input=1; 
      r.samples[i].ulpError=(s&7)?1:0; r.samples[i].mismatch=(s&7)!=0; }
    r.total=n; r.mismatchCount=n/8; r.finalise();

    UlpPlot p; p.resize(1200,400); p.setResult(&r);
    QPixmap pm(p.size()); pm.fill(Qt::white);
    auto timeIt=[&](const char*label,auto fn,int reps){
      QElapsedTimer t; t.start();
      for(int i=0;i<reps;i++) fn();
      qint64 el=t.nsecsElapsed()/reps;
      // Count painted pixels too: a fast no-op would otherwise look like a
      // great result. This exact trap is why the check is here.
      pm.fill(Qt::white); p.render(&pm);
      QImage img = pm.toImage();
      long drawn = 0;
      for(int y=0;y<img.height();y+=2) for(int x=0;x<img.width();x+=2)
        if(img.pixel(x,y)!=0xffffffffu) ++drawn;
      std::printf("  %-10llu samples  %-8s %7lld us/op  %7ld px\n",
        (unsigned long long)n, label, (long long)(el/1000), drawn);
    };
    timeIt("paint", [&]{ p.render(&pm); }, 20);
  }
  return 0;
}
