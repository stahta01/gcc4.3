
// DO NOT EDIT THIS FILE - it is machine generated -*- c++ -*-

#ifndef __gnu_xml_transform_AttributeNode__
#define __gnu_xml_transform_AttributeNode__

#pragma interface

#include <gnu/xml/transform/TemplateNode.h>
extern "Java"
{
  namespace gnu
  {
    namespace xml
    {
      namespace transform
      {
          class AttributeNode;
          class Stylesheet;
          class TemplateNode;
      }
    }
  }
  namespace javax
  {
    namespace xml
    {
      namespace namespace
      {
          class QName;
      }
    }
  }
  namespace org
  {
    namespace w3c
    {
      namespace dom
      {
          class Node;
      }
    }
  }
}

class gnu::xml::transform::AttributeNode : public ::gnu::xml::transform::TemplateNode
{

public: // actually package-private
  AttributeNode(::gnu::xml::transform::TemplateNode *, ::gnu::xml::transform::TemplateNode *, ::org::w3c::dom::Node *);
  ::gnu::xml::transform::TemplateNode * clone(::gnu::xml::transform::Stylesheet *);
  void doApply(::gnu::xml::transform::Stylesheet *, ::javax::xml::namespace::QName *, ::org::w3c::dom::Node *, jint, jint, ::org::w3c::dom::Node *, ::org::w3c::dom::Node *);
  ::java::lang::String * getPrefix(::java::lang::String *);
  ::java::lang::String * inventPrefix(::org::w3c::dom::Node *);
public:
  jboolean references(::javax::xml::namespace::QName *);
  ::java::lang::String * toString();
public: // actually package-private
  ::gnu::xml::transform::TemplateNode * __attribute__((aligned(__alignof__( ::gnu::xml::transform::TemplateNode)))) name;
  ::gnu::xml::transform::TemplateNode * namespace$;
  ::org::w3c::dom::Node * source;
public:
  static ::java::lang::Class class$;
};

#endif // __gnu_xml_transform_AttributeNode__
