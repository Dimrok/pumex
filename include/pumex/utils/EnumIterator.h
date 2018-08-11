#pragma once

#include <type_traits>

namespace pumex
{
// Helper class that enables iterating over modern C++ enums, assuming values
// are contiguous.
// Found this gem on Stack Overflow : https://stackoverflow.com/questions/261963/how-can-i-iterate-over-an-enum
template < typename C, C beginVal, C endVal>
class EnumIterator
{
  using val_t = typename std::underlying_type<C>::type;

  val_t val;

public:
  /// Create an iterator starting at a given value.
  EnumIterator(const C & f = beginVal)
    : val(static_cast<val_t>(f))
  {
  }

  EnumIterator operator++()
  {
    ++val;
    return *this;
  }

  C operator*()
  {
    return static_cast<C>(val);
  }

  EnumIterator begin()
  {
    return *this;
  }

  EnumIterator end()
  {
    static const EnumIterator endIter = ++EnumIterator(endVal); // cache it
    return endIter;
  }

  bool operator!=(const EnumIterator& i)
  {
    return val != i.val;
  }
};
}
