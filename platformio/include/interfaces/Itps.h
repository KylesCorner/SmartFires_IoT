#pragma once

class Itps{
public:
  virtual ~Itps() = default;

  virtual bool begin() = 0;
  virtual bool enable() = 0;
  virtual bool disable() = 0;
  virtual bool enabled() const = 0;
};
