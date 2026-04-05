#ifndef ROBFLEX_EVENT_H
#define ROBFLEX_EVENT_H

#include <stdint.h>
#include <string.h>

typedef uint32_t RobflexEventMask;

enum RobflexEventIdx {
    ROBFLEX_EVENT_NONE = 0,
    ROBFLEX_EVENT_OBSTACLE_DENSE,
    ROBFLEX_EVENT_PEDESTRIAN_DENSE,
    ROBFLEX_EVENT_HIGH_SPEED,
    ROBFLEX_EVENT_CRUISE_NORMAL,
    ROBFLEX_EVENT_STATIONARY,
    ROBFLEX_EVENT_INTERACTION,
    ROBFLEX_EVENT_AWAITING_COMMAND,
    ROBFLEX_EVENT_EMERGENCY_STOP,
};

static const char *const robflex_event_name_list[] = {
    "obstacle_dense",
    "pedestrian_dense",
    "high_speed",
    "cruise_normal",
    "stationary",
    "interaction",
    "awaiting_command",
    "emergency_stop",
};

#define ROBFLEX_EVENT_NAME_COUNT \
  (sizeof(robflex_event_name_list) / sizeof(robflex_event_name_list[0]))

// TODO: get event idx from server
// Transform event_name => event_idx
static inline enum RobflexEventIdx robflex_get_event_idx(const char *event_name)
{
  size_t i;

  if (event_name == NULL) {
    return ROBFLEX_EVENT_NONE;
  }
  for (i = 0; i < ROBFLEX_EVENT_NAME_COUNT; i++) {
    if (strcmp(event_name, robflex_event_name_list[i]) == 0) {
      return (enum RobflexEventIdx)(i + 1);
    }
  }
  return ROBFLEX_EVENT_NONE;
}

#endif // ROBFLEX_EVENT_H