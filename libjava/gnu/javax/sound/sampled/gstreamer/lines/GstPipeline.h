
// DO NOT EDIT THIS FILE - it is machine generated -*- c++ -*-

#ifndef __gnu_javax_sound_sampled_gstreamer_lines_GstPipeline__
#define __gnu_javax_sound_sampled_gstreamer_lines_GstPipeline__

#pragma interface

#include <java/lang/Object.h>
extern "Java"
{
  namespace gnu
  {
    namespace classpath
    {
        class Pointer;
    }
    namespace javax
    {
      namespace sound
      {
        namespace sampled
        {
          namespace gstreamer
          {
            namespace lines
            {
                class GstPipeline;
            }
          }
        }
      }
    }
  }
}

class gnu::javax::sound::sampled::gstreamer::lines::GstPipeline : public ::java::lang::Object
{

public:
  GstPipeline();
private:
  void setState();
  ::gnu::classpath::Pointer * __attribute__((aligned(__alignof__( ::java::lang::Object)))) pipeline;
public:
  static ::java::lang::Class class$;
};

#endif // __gnu_javax_sound_sampled_gstreamer_lines_GstPipeline__
