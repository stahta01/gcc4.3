
// DO NOT EDIT THIS FILE - it is machine generated -*- c++ -*-

#ifndef __gnu_javax_naming_giop_GiopNamingServiceFactory$1__
#define __gnu_javax_naming_giop_GiopNamingServiceFactory$1__

#pragma interface

#include <java/lang/Thread.h>
extern "Java"
{
  namespace gnu
  {
    namespace javax
    {
      namespace naming
      {
        namespace giop
        {
            class GiopNamingServiceFactory;
            class GiopNamingServiceFactory$1;
        }
      }
    }
  }
  namespace org
  {
    namespace omg
    {
      namespace CORBA
      {
          class ORB;
      }
    }
  }
}

class gnu::javax::naming::giop::GiopNamingServiceFactory$1 : public ::java::lang::Thread
{

public: // actually package-private
  GiopNamingServiceFactory$1(::gnu::javax::naming::giop::GiopNamingServiceFactory *, ::org::omg::CORBA::ORB *);
public:
  void run();
public: // actually package-private
  ::gnu::javax::naming::giop::GiopNamingServiceFactory * __attribute__((aligned(__alignof__( ::java::lang::Thread)))) this$0;
private:
  ::org::omg::CORBA::ORB * val$runIt;
public:
  static ::java::lang::Class class$;
};

#endif // __gnu_javax_naming_giop_GiopNamingServiceFactory$1__
