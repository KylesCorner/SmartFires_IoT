// ---
// description: Abstraction for enabling/disabling a TPS (throttle/sensor power-switch style) line.
// role: interface
// ---
#pragma once

class Itps{
public:
  virtual ~Itps() = default;

  virtual bool begin() = 0;
  virtual bool enable() = 0;
  virtual bool disable() = 0;
  virtual bool enabled() const = 0;
};
