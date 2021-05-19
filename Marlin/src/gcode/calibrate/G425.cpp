/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "../../MarlinCore.h"

#if ENABLED(CALIBRATION_GCODE)

#include "../gcode.h"

#if ENABLED(BACKLASH_GCODE)
  #include "../../feature/backlash.h"
#endif

#include "../../lcd/marlinui.h"
#include "../../module/motion.h"
#include "../../module/planner.h"
#include "../../module/tool_change.h"
#include "../../module/endstops.h"
#include "../../feature/bedlevel/bedlevel.h"

#if !AXIS_CAN_CALIBRATE(X)
  #undef CALIBRATION_MEASURE_LEFT
  #undef CALIBRATION_MEASURE_RIGHT
#endif

#if !AXIS_CAN_CALIBRATE(Y)
  #undef CALIBRATION_MEASURE_FRONT
  #undef CALIBRATION_MEASURE_BACK
#endif

#if !AXIS_CAN_CALIBRATE(Z)
  #undef CALIBRATION_MEASURE_AT_TOP_EDGES
#endif

/**
 * G425 backs away from the calibration object by various distances
 * depending on the confidence level:
 *
 *   UNKNOWN   - No real notion on where the calibration object is on the bed
 *   UNCERTAIN - Measurement may be uncertain due to backlash
 *   CERTAIN   - Measurement obtained with backlash compensation
 */

#ifndef CALIBRATION_MEASUREMENT_UNKNOWN
  #define CALIBRATION_MEASUREMENT_UNKNOWN   5.0 // mm
#endif
#ifndef CALIBRATION_MEASUREMENT_UNCERTAIN
  #define CALIBRATION_MEASUREMENT_UNCERTAIN 1.0 // mm
#endif
#ifndef CALIBRATION_MEASUREMENT_CERTAIN
  #define CALIBRATION_MEASUREMENT_CERTAIN   0.5 // mm
#endif

#if BOTH(CALIBRATION_MEASURE_LEFT, CALIBRATION_MEASURE_RIGHT)
  #define HAS_X_CENTER 1
#endif
#if BOTH(CALIBRATION_MEASURE_FRONT, CALIBRATION_MEASURE_BACK)
  #define HAS_Y_CENTER 1
#endif
#if LINEAR_AXES >= 4 && BOTH(CALIBRATION_MEASURE_IMIN, CALIBRATION_MEASURE_IMAX)
  #define HAS_I_CENTER 1
#endif
#if LINEAR_AXES >= 5 && BOTH(CALIBRATION_MEASURE_JMIN, CALIBRATION_MEASURE_JMAX)
  #define HAS_J_CENTER 1
#endif
#if LINEAR_AXES >= 6 && BOTH(CALIBRATION_MEASURE_KMIN, CALIBRATION_MEASURE_KMAX)
  #define HAS_K_CENTER 1
#endif
#if LINEAR_AXES >= 7 && BOTH(CALIBRATION_MEASURE_MMIN, CALIBRATION_MEASURE_MMAX)    /**SG**/
  #define HAS_M_CENTER 1
#endif
#if LINEAR_AXES >= 8 && BOTH(CALIBRATION_MEASURE_OMIN, CALIBRATION_MEASURE_OMAX)    /**SG**/
  #define HAS_O_CENTER 1
#endif
#if LINEAR_AXES >= 9 && BOTH(CALIBRATION_MEASURE_PMIN, CALIBRATION_MEASURE_PMAX)    /**SG**/
  #define HAS_P_CENTER 1
#endif
#if LINEAR_AXES >= 10 && BOTH(CALIBRATION_MEASURE_QMIN, CALIBRATION_MEASURE_QMAX)    /**SG**/
  #define HAS_Q_CENTER 1
#endif
enum side_t : uint8_t {
  TOP, RIGHT, FRONT, LEFT, BACK, NUM_SIDES,
  LIST_N(DOUBLE(SUB3(LINEAR_AXES)), IMINIMUM, IMAXIMUM, JMINIMUM, JMAXIMUM, KMINIMUM, KMAXIMUM, MMINIMUM, MMAXIMUM, OMINIMUM, OMAXIMUM, PMINIMUM, PMAXIMUM, QMINIMUM, QMAXIMUM)  /**SG**/
};

static constexpr xyz_pos_t true_center CALIBRATION_OBJECT_CENTER;
static constexpr xyz_float_t dimensions CALIBRATION_OBJECT_DIMENSIONS;
static constexpr xy_float_t nod = { CALIBRATION_NOZZLE_OUTER_DIAMETER, CALIBRATION_NOZZLE_OUTER_DIAMETER };

struct measurements_t {
  xyz_pos_t obj_center = true_center; // Non-static must be assigned from xyz_pos_t

  float obj_side[NUM_SIDES], backlash[NUM_SIDES];
  xyz_float_t pos_error;

  xy_float_t nozzle_outer_dimension = nod;
};

#if ENABLED(BACKLASH_GCODE)
  #define TEMPORARY_BACKLASH_CORRECTION(value) REMEMBER(tbst, backlash.correction, value)
#else
  #define TEMPORARY_BACKLASH_CORRECTION(value)
#endif

#if ENABLED(BACKLASH_GCODE) && defined(BACKLASH_SMOOTHING_MM)
  #define TEMPORARY_BACKLASH_SMOOTHING(value) REMEMBER(tbsm, backlash.smoothing_mm, value)
#else
  #define TEMPORARY_BACKLASH_SMOOTHING(value)
#endif

inline void calibration_move() {
  do_blocking_move_to((xyz_pos_t)current_position, MMM_TO_MMS(CALIBRATION_FEEDRATE_TRAVEL));
}

/**
 * Move to the exact center above the calibration object
 *
 *   m                  in     - Measurement record
 *   uncertainty        in     - How far away from the object top to park
 */
inline void park_above_object(measurements_t &m, const float uncertainty) {
  // Move to safe distance above calibration object
  current_position.z = m.obj_center.z + dimensions.z / 2 + uncertainty;
  calibration_move();

  // Move to center of calibration object in XY
  current_position = xy_pos_t(m.obj_center);
  calibration_move();
}

#if HAS_MULTI_HOTEND
  inline void set_nozzle(measurements_t &m, const uint8_t extruder) {
    if (extruder != active_extruder) {
      park_above_object(m, CALIBRATION_MEASUREMENT_UNKNOWN);
      tool_change(extruder);
    }
  }
#endif

#if HAS_HOTEND_OFFSET

  inline void normalize_hotend_offsets() {
    LOOP_S_L_N(e, 1, HOTENDS)
      hotend_offset[e] -= hotend_offset[0];
    hotend_offset[0].reset();
  }

#endif

#if !PIN_EXISTS(CALIBRATION)
  #include "../../module/probe.h"
#endif

inline bool read_calibration_pin() {
  return (
    #if PIN_EXISTS(CALIBRATION)
      READ(CALIBRATION_PIN) != CALIBRATION_PIN_INVERTING
    #else
      PROBE_TRIGGERED()
    #endif
  );
}

/**
 * Move along axis in the specified dir until the probe value becomes stop_state,
 * then return the axis value.
 *
 *   axis         in - Axis along which the measurement will take place
 *   dir          in - Direction along that axis (-1 or 1)
 *   stop_state   in - Move until probe pin becomes this value
 *   fast         in - Fast vs. precise measurement
 */
float measuring_movement(const AxisEnum axis, const int dir, const bool stop_state, const bool fast) {
  const float step     = fast ? 0.25 : CALIBRATION_MEASUREMENT_RESOLUTION;
  const feedRate_t mms = fast ? MMM_TO_MMS(CALIBRATION_FEEDRATE_FAST) : MMM_TO_MMS(CALIBRATION_FEEDRATE_SLOW);
  const float limit    = fast ? 50 : 5;

  destination = current_position;
  for (float travel = 0; travel < limit; travel += step) {
    destination[axis] += dir * step;
    do_blocking_move_to((xyz_pos_t)destination, mms);
    planner.synchronize();
    if (read_calibration_pin() == stop_state) break;
  }
  return destination[axis];
}

/**
 * Move along axis until the probe is triggered. Move toolhead to its starting
 * point and return the measured value.
 *
 *   axis               in     - Axis along which the measurement will take place
 *   dir                in     - Direction along that axis (-1 or 1)
 *   stop_state         in     - Move until probe pin becomes this value
 *   backlash_ptr       in/out - When not nullptr, measure and record axis backlash
 *   uncertainty        in     - If uncertainty is CALIBRATION_MEASUREMENT_UNKNOWN, do a fast probe.
 */
inline float measure(const AxisEnum axis, const int dir, const bool stop_state, float * const backlash_ptr, const float uncertainty) {
  const bool fast = uncertainty == CALIBRATION_MEASUREMENT_UNKNOWN;

  // Save the current position of the specified axis
  const float start_pos = current_position[axis];

  // Take a measurement. Only the specified axis will be affected.
  const float measured_pos = measuring_movement(axis, dir, stop_state, fast);

  // Measure backlash
  if (backlash_ptr && !fast) {
    const float release_pos = measuring_movement(axis, -dir, !stop_state, fast);
    *backlash_ptr = ABS(release_pos - measured_pos);
  }

  // Move back to the starting position
  destination = current_position;
  destination[axis] = start_pos;
  do_blocking_move_to((xyz_pos_t)destination, MMM_TO_MMS(CALIBRATION_FEEDRATE_TRAVEL));
  return measured_pos;
}

/**
 * Probe one side of the calibration object
 *
 *   m                  in/out - Measurement record, m.obj_center and m.obj_side will be updated.
 *   uncertainty        in     - How far away from the calibration object to begin probing
 *   side               in     - Side of probe where probe will occur
 *   probe_top_at_edge  in     - When probing sides, probe top of calibration object nearest edge
 *                               to find out height of edge
 */
inline void probe_side(measurements_t &m, const float uncertainty, const side_t side, const bool probe_top_at_edge=false) {
  const xyz_float_t dimensions = CALIBRATION_OBJECT_DIMENSIONS;
  AxisEnum axis;
  float dir = 1;

  park_above_object(m, uncertainty);

  switch (side) {
    #if AXIS_CAN_CALIBRATE(Z)
      case TOP: {
        const float measurement = measure(Z_AXIS, -1, true, &m.backlash[TOP], uncertainty);
        m.obj_center.z = measurement - dimensions.z / 2;
        m.obj_side[TOP] = measurement;
        return;
      }
    #endif
    #if AXIS_CAN_CALIBRATE(X)
      case RIGHT: dir = -1;
      case LEFT:  axis = X_AXIS; break;
    #endif
    #if AXIS_CAN_CALIBRATE(Y)
      case BACK:  dir = -1;
      case FRONT: axis = Y_AXIS; break;
    #endif
    #if LINEAR_AXES >= 4
      case IMINIMUM: dir = -1;
      case IMAXIMUM: axis = I_AXIS; break;
    #endif
    #if LINEAR_AXES >= 5
      case JMINIMUM: dir = -1;
      case JMAXIMUM: axis = J_AXIS; break;
    #endif
    #if LINEAR_AXES >= 6
      case KMINIMUM: dir = -1;
      case KMAXIMUM: axis = K_AXIS; break;
    #endif
    #if LINEAR_AXES >= 7    /**SG**/
      case MMINIMUM: dir = -1;
      case MMAXIMUM: axis = M_AXIS; break;
    #endif
    #if LINEAR_AXES >= 8    /**SG**/
      case OMINIMUM: dir = -1;
      case OMAXIMUM: axis = O_AXIS; break;
    #endif
    #if LINEAR_AXES >= 9    /**SG**/
      case PMINIMUM: dir = -1;
      case PMAXIMUM: axis = P_AXIS; break;
    #endif
    #if LINEAR_AXES >= 10    /**SG**/
      case QMINIMUM: dir = -1;
      case QMAXIMUM: axis = Q_AXIS; break;
    #endif
    default: return;
  }

  if (probe_top_at_edge) {
    #if AXIS_CAN_CALIBRATE(Z)
      // Probe top nearest the side we are probing
      current_position[axis] = m.obj_center[axis] + (-dir) * (dimensions[axis] / 2 - m.nozzle_outer_dimension[axis]);
      calibration_move();
      m.obj_side[TOP] = measure(Z_AXIS, -1, true, &m.backlash[TOP], uncertainty);
      m.obj_center.z = m.obj_side[TOP] - dimensions.z / 2;
    #endif
  }

  if ((AXIS_CAN_CALIBRATE(X) && axis == X_AXIS) || (AXIS_CAN_CALIBRATE(Y) && axis == Y_AXIS)) {
    // Move to safe distance to the side of the calibration object
    current_position[axis] = m.obj_center[axis] + (-dir) * (dimensions[axis] / 2 + m.nozzle_outer_dimension[axis] / 2 + uncertainty);
    calibration_move();

    // Plunge below the side of the calibration object and measure
    current_position.z = m.obj_side[TOP] - (CALIBRATION_NOZZLE_TIP_HEIGHT) * 0.7f;
    calibration_move();
    const float measurement = measure(axis, dir, true, &m.backlash[side], uncertainty);
    m.obj_center[axis] = measurement + dir * (dimensions[axis] / 2 + m.nozzle_outer_dimension[axis] / 2);
    m.obj_side[side] = measurement;
  }
}

/**
 * Probe all sides of the calibration calibration object
 *
 *   m                  in/out - Measurement record: center, backlash and error values be updated.
 *   uncertainty        in     - How far away from the calibration object to begin probing
 */
inline void probe_sides(measurements_t &m, const float uncertainty) {
  #if ENABLED(CALIBRATION_MEASURE_AT_TOP_EDGES)
    constexpr bool probe_top_at_edge = true;
  #else
    // Probing at the exact center only works if the center is flat. Probing on a washer
    // or bolt will require probing the top near the side edges, away from the center.
    constexpr bool probe_top_at_edge = false;
    probe_side(m, uncertainty, TOP);
  #endif

  TERN_(CALIBRATION_MEASURE_RIGHT, probe_side(m, uncertainty, RIGHT, probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_FRONT, probe_side(m, uncertainty, FRONT, probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_LEFT,  probe_side(m, uncertainty, LEFT,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_BACK,  probe_side(m, uncertainty, BACK,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_IMIN,  probe_side(m, uncertainty, IMINIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_IMAX,  probe_side(m, uncertainty, IMAXIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_JMIN,  probe_side(m, uncertainty, JMINIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_JMAX,  probe_side(m, uncertainty, JMAXIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_KMIN,  probe_side(m, uncertainty, KMINIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_KMAX,  probe_side(m, uncertainty, KMAXIMUM,  probe_top_at_edge));
  TERN_(CALIBRATION_MEASURE_MMIN,  probe_side(m, uncertainty, MMINIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_MMAX,  probe_side(m, uncertainty, MMAXIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_OMIN,  probe_side(m, uncertainty, OMINIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_OMAX,  probe_side(m, uncertainty, OMAXIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_PMIN,  probe_side(m, uncertainty, PMINIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_PMAX,  probe_side(m, uncertainty, PMAXIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_QMIN,  probe_side(m, uncertainty, QMINIMUM,  probe_top_at_edge));   /**SG**/
  TERN_(CALIBRATION_MEASURE_QMAX,  probe_side(m, uncertainty, QMAXIMUM,  probe_top_at_edge));   /**SG**/

  // Compute the measured center of the calibration object.
  TERN_(HAS_X_CENTER, m.obj_center.x = (m.obj_side[LEFT] + m.obj_side[RIGHT]) / 2);
  TERN_(HAS_Y_CENTER, m.obj_center.y = (m.obj_side[FRONT] + m.obj_side[BACK]) / 2);
  TERN_(HAS_I_CENTER, m.obj_center.i = (m.obj_side[IMINIMUM] + m.obj_side[IMAXIMUM]) / 2);
  TERN_(HAS_J_CENTER, m.obj_center.j = (m.obj_side[JMINIMUM] + m.obj_side[JMAXIMUM]) / 2);
  TERN_(HAS_K_CENTER, m.obj_center.k = (m.obj_side[KMINIMUM] + m.obj_side[KMAXIMUM]) / 2);
  TERN_(HAS_M_CENTER, m.obj_center.m = (m.obj_side[MMINIMUM] + m.obj_side[MMAXIMUM]) / 2);    /**SG**/
  TERN_(HAS_O_CENTER, m.obj_center.o = (m.obj_side[OMINIMUM] + m.obj_side[OMAXIMUM]) / 2);    /**SG**/
  TERN_(HAS_P_CENTER, m.obj_center.p = (m.obj_side[PMINIMUM] + m.obj_side[PMAXIMUM]) / 2);    /**SG**/
  TERN_(HAS_Q_CENTER, m.obj_center.q = (m.obj_side[QMINIMUM] + m.obj_side[QMAXIMUM]) / 2);    /**SG**/

  // Compute the outside diameter of the nozzle at the height
  // at which it makes contact with the calibration object
  TERN_(HAS_X_CENTER, m.nozzle_outer_dimension.x = m.obj_side[RIGHT] - m.obj_side[LEFT] - dimensions.x);
  TERN_(HAS_Y_CENTER, m.nozzle_outer_dimension.y = m.obj_side[BACK]  - m.obj_side[FRONT] - dimensions.y);

  park_above_object(m, uncertainty);

  // The difference between the known and the measured location
  // of the calibration object is the positional error
  m.pos_error.x = TERN0(HAS_X_CENTER, true_center.x - m.obj_center.x);
  m.pos_error.y = TERN0(HAS_Y_CENTER, true_center.y - m.obj_center.y);
  m.pos_error.z = true_center.z - m.obj_center.z;
  #if LINEAR_AXES >= 4
    m.pos_error.i = TERN0(HAS_I_CENTER, true_center.i - m.obj_center.i);
  #endif
  #if LINEAR_AXES >= 5
    m.pos_error.j = TERN0(HAS_J_CENTER, true_center.j - m.obj_center.j);
  #endif
  #if LINEAR_AXES >= 6
    m.pos_error.k = TERN0(HAS_K_CENTER, true_center.k - m.obj_center.k);
  #endif
  #if LINEAR_AXES >= 7    /**SG**/
    m.pos_error.m = TERN0(HAS_M_CENTER, true_center.m - m.obj_center.m);
  #endif
  #if LINEAR_AXES >= 8    /**SG**/
    m.pos_error.o = TERN0(HAS_O_CENTER, true_center.o - m.obj_center.o);
  #endif
  #if LINEAR_AXES >= 9    /**SG**/
    m.pos_error.p = TERN0(HAS_P_CENTER, true_center.p - m.obj_center.p);
  #endif
  #if LINEAR_AXES >= 10    /**SG**/
    m.pos_error.q = TERN0(HAS_Q_CENTER, true_center.q - m.obj_center.q);
  #endif
}

#if ENABLED(CALIBRATION_REPORTING)
  inline void report_measured_faces(const measurements_t &m) {
    SERIAL_ECHOLNPGM("Sides:");
    #if AXIS_CAN_CALIBRATE(Z)
      SERIAL_ECHOLNPAIR("  Top: ", m.obj_side[TOP]);
    #endif
    #if ENABLED(CALIBRATION_MEASURE_LEFT)
      SERIAL_ECHOLNPAIR("  Left: ", m.obj_side[LEFT]);
    #endif
    #if ENABLED(CALIBRATION_MEASURE_RIGHT)
      SERIAL_ECHOLNPAIR("  Right: ", m.obj_side[RIGHT]);
    #endif
    #if ENABLED(CALIBRATION_MEASURE_FRONT)
      SERIAL_ECHOLNPAIR("  Front: ", m.obj_side[FRONT]);
    #endif
    #if ENABLED(CALIBRATION_MEASURE_BACK)
      SERIAL_ECHOLNPAIR("  Back: ", m.obj_side[BACK]);
    #endif
    #if LINEAR_AXES >= 4
      #if ENABLED(CALIBRATION_MEASURE_IMIN)
        SERIAL_ECHOLNPAIR("  Imin: ", m.obj_side[IMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_IMAX)
        SERIAL_ECHOLNPAIR("  Imax: ", m.obj_side[IMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 5
      #if ENABLED(CALIBRATION_MEASURE_JMIN)
        SERIAL_ECHOLNPAIR("  Jmin: ", m.obj_side[JMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_JMAX)
        SERIAL_ECHOLNPAIR("  Jmax: ", m.obj_side[JMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 6
      #if ENABLED(CALIBRATION_MEASURE_KMIN)
        SERIAL_ECHOLNPAIR("  Kmin: ", m.obj_side[KMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_KMAX)
        SERIAL_ECHOLNPAIR("  Kmax: ", m.obj_side[KMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 7    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_MMIN)
        SERIAL_ECHOLNPAIR("  Mmin: ", m.obj_side[MMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_MMAX)
        SERIAL_ECHOLNPAIR("  Mmax: ", m.obj_side[MMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 8    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_OMIN)
        SERIAL_ECHOLNPAIR("  Omin: ", m.obj_side[OMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_OMAX)
        SERIAL_ECHOLNPAIR("  Omax: ", m.obj_side[OMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 9    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_PMIN)
        SERIAL_ECHOLNPAIR("  Pmin: ", m.obj_side[PMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_PMAX)
        SERIAL_ECHOLNPAIR("  Pmax: ", m.obj_side[PMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 10    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_QMIN)
        SERIAL_ECHOLNPAIR("  Qmin: ", m.obj_side[QMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_QMAX)
        SERIAL_ECHOLNPAIR("  Qmax: ", m.obj_side[QMAXIMUM]);
      #endif
    #endif

    SERIAL_EOL();
  }

  inline void report_measured_center(const measurements_t &m) {
    SERIAL_ECHOLNPGM("Center:");
    #if HAS_X_CENTER
      SERIAL_ECHOLNPAIR_P(SP_X_STR, m.obj_center.x);
    #endif
    #if HAS_Y_CENTER
      SERIAL_ECHOLNPAIR_P(SP_Y_STR, m.obj_center.y);
    #endif
    SERIAL_ECHOLNPAIR_P(SP_Z_STR, m.obj_center.z);
    #if HAS_I_CENTER
      SERIAL_ECHOLNPAIR_P(SP_I_STR, m.obj_center.i);
    #endif
    #if HAS_J_CENTER
      SERIAL_ECHOLNPAIR_P(SP_J_STR, m.obj_center.j);
    #endif
    #if HAS_K_CENTER
      SERIAL_ECHOLNPAIR_P(SP_K_STR, m.obj_center.k);
    #endif
    #if HAS_M_CENTER      /**SG**/
      SERIAL_ECHOLNPAIR_P(SP_M_STR, m.obj_center.m);
    #endif
    #if HAS_O_CENTER      /**SG**/
      SERIAL_ECHOLNPAIR_P(SP_O_STR, m.obj_center.o);
    #endif
    #if HAS_P_CENTER      /**SG**/
      SERIAL_ECHOLNPAIR_P(SP_P_STR, m.obj_center.p);
    #endif
    #if HAS_Q_CENTER      /**SG**/
      SERIAL_ECHOLNPAIR_P(SP_Q_STR, m.obj_center.q);
    #endif
    SERIAL_EOL();
  }

  inline void report_measured_backlash(const measurements_t &m) {
    SERIAL_ECHOLNPGM("Backlash:");
    #if AXIS_CAN_CALIBRATE(X)
      #if ENABLED(CALIBRATION_MEASURE_LEFT)
        SERIAL_ECHOLNPAIR("  Left: ", m.backlash[LEFT]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_RIGHT)
        SERIAL_ECHOLNPAIR("  Right: ", m.backlash[RIGHT]);
      #endif
    #endif
    #if AXIS_CAN_CALIBRATE(Y)
      #if ENABLED(CALIBRATION_MEASURE_FRONT)
        SERIAL_ECHOLNPAIR("  Front: ", m.backlash[FRONT]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_BACK)
        SERIAL_ECHOLNPAIR("  Back: ", m.backlash[BACK]);
      #endif
    #endif
    #if AXIS_CAN_CALIBRATE(Z)
      SERIAL_ECHOLNPAIR("  Top: ", m.backlash[TOP]);
    #endif
    #if LINEAR_AXES >= 4
      #if ENABLED(CALIBRATION_MEASURE_IMIN)
        SERIAL_ECHOLNPAIR("  Imin: ", m.backlash[IMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_IMAX)
        SERIAL_ECHOLNPAIR("  Imax: ", m.backlash[IMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 5
      #if ENABLED(CALIBRATION_MEASURE_JMIN)
        SERIAL_ECHOLNPAIR("  Jmin: ", m.backlash[JMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_JMAX)
        SERIAL_ECHOLNPAIR("  Jmax: ", m.backlash[JMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 6
      #if ENABLED(CALIBRATION_MEASURE_KMIN)
        SERIAL_ECHOLNPAIR("  Kmin: ", m.backlash[KMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_KMAX)
        SERIAL_ECHOLNPAIR("  Kmax: ", m.backlash[KMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 7    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_MMIN)
        SERIAL_ECHOLNPAIR("  Mmin: ", m.backlash[MMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_MMAX)
        SERIAL_ECHOLNPAIR("  Mmax: ", m.backlash[MMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 8    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_OMIN)
        SERIAL_ECHOLNPAIR("  Omin: ", m.backlash[OMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_OMAX)
        SERIAL_ECHOLNPAIR("  Omax: ", m.backlash[OMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 9    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_PMIN)
        SERIAL_ECHOLNPAIR("  Pmin: ", m.backlash[PMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_PMAX)
        SERIAL_ECHOLNPAIR("  Pmax: ", m.backlash[PMAXIMUM]);
      #endif
    #endif
    #if LINEAR_AXES >= 10    /**SG**/
      #if ENABLED(CALIBRATION_MEASURE_QMIN)
        SERIAL_ECHOLNPAIR("  Qmin: ", m.backlash[QMINIMUM]);
      #endif
      #if ENABLED(CALIBRATION_MEASURE_QMAX)
        SERIAL_ECHOLNPAIR("  Qmax: ", m.backlash[QMAXIMUM]);
      #endif
    #endif
    SERIAL_EOL();
  }

  inline void report_measured_positional_error(const measurements_t &m) {
    SERIAL_CHAR('T');
    SERIAL_ECHO(int(active_extruder));
    SERIAL_ECHOLNPGM(" Positional Error:");
    #if HAS_X_CENTER
      SERIAL_ECHOLNPAIR_P(SP_X_STR, m.pos_error.x);
    #endif
    #if HAS_Y_CENTER
      SERIAL_ECHOLNPAIR_P(SP_Y_STR, m.pos_error.y);
    #endif
    if (AXIS_CAN_CALIBRATE(Z)) SERIAL_ECHOLNPAIR_P(SP_Z_STR, m.pos_error.z);
    SERIAL_EOL();
  }

  inline void report_measured_nozzle_dimensions(const measurements_t &m) {
    SERIAL_ECHOLNPGM("Nozzle Tip Outer Dimensions:");
    #if HAS_X_CENTER || HAS_Y_CENTER
      #if HAS_X_CENTER
        SERIAL_ECHOLNPAIR_P(SP_X_STR, m.nozzle_outer_dimension.x);
      #endif
      #if HAS_Y_CENTER
        SERIAL_ECHOLNPAIR_P(SP_Y_STR, m.nozzle_outer_dimension.y);
      #endif
    #else
      UNUSED(m);
    #endif
    SERIAL_EOL();
  }

  #if HAS_HOTEND_OFFSET
    //
    // This function requires normalize_hotend_offsets() to be called
    //
    inline void report_hotend_offsets() {
      LOOP_S_L_N(e, 1, HOTENDS)
        SERIAL_ECHOLNPAIR_P(PSTR("T"), int(e), PSTR(" Hotend Offset X"), hotend_offset[e].x, SP_Y_STR, hotend_offset[e].y, SP_Z_STR, hotend_offset[e].z);
    }
  #endif

#endif // CALIBRATION_REPORTING

/**
 * Probe around the calibration object to measure backlash
 *
 *   m              in/out - Measurement record, updated with new readings
 *   uncertainty    in     - How far away from the object to begin probing
 */
inline void calibrate_backlash(measurements_t &m, const float uncertainty) {
  // Backlash compensation should be off while measuring backlash

  {
    // New scope for TEMPORARY_BACKLASH_CORRECTION
    TEMPORARY_BACKLASH_CORRECTION(all_off);
    TEMPORARY_BACKLASH_SMOOTHING(0.0f);

    probe_sides(m, uncertainty);

    #if ENABLED(BACKLASH_GCODE)

      #if HAS_X_CENTER
        backlash.distance_mm.x = (m.backlash[LEFT] + m.backlash[RIGHT]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_LEFT)
        backlash.distance_mm.x = m.backlash[LEFT];
      #elif ENABLED(CALIBRATION_MEASURE_RIGHT)
        backlash.distance_mm.x = m.backlash[RIGHT];
      #endif

      #if HAS_Y_CENTER
        backlash.distance_mm.y = (m.backlash[FRONT] + m.backlash[BACK]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_FRONT)
        backlash.distance_mm.y = m.backlash[FRONT];
      #elif ENABLED(CALIBRATION_MEASURE_BACK)
        backlash.distance_mm.y = m.backlash[BACK];
      #endif

      if (AXIS_CAN_CALIBRATE(Z)) backlash.distance_mm.z = m.backlash[TOP];

      #if HAS_I_CENTER
        backlash.distance_mm.i = (m.backlash[IMINIMUM] + m.backlash[IMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_IMIN)
        backlash.distance_mm.i = m.backlash[IMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_IMAX)
        backlash.distance_mm.i = m.backlash[IMAXIMUM];
      #endif

      #if HAS_J_CENTER
        backlash.distance_mm.j = (m.backlash[JMINIMUM] + m.backlash[JMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_JMIN)
        backlash.distance_mm.j = m.backlash[JMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_JMAX)
        backlash.distance_mm.j = m.backlash[JMAXIMUM];
      #endif

      #if HAS_K_CENTER
        backlash.distance_mm.k = (m.backlash[KMINIMUM] + m.backlash[KMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_KMIN)
        backlash.distance_mm.k = m.backlash[KMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_KMAX)
        backlash.distance_mm.k = m.backlash[KMAXIMUM];
      #endif

      #if HAS_M_CENTER    /**SG**/
        backlash.distance_mm.m = (m.backlash[MMINIMUM] + m.backlash[MMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_MMIN)
        backlash.distance_mm.m = m.backlash[MMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_MMAX)
        backlash.distance_mm.m = m.backlash[MMAXIMUM];
      #endif
      #if HAS_O_CENTER    /**SG**/
        backlash.distance_mm.o = (m.backlash[OMINIMUM] + m.backlash[OMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_OMIN)
        backlash.distance_mm.o = m.backlash[OMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_MMAX)
        backlash.distance_mm.o = m.backlash[OMAXIMUM];
      #endif
      #if HAS_P_CENTER    /**SG**/
        backlash.distance_mm.p = (m.backlash[PMINIMUM] + m.backlash[PMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_PMIN)
        backlash.distance_mm.p = m.backlash[PMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_PMAX)
        backlash.distance_mm.p = m.backlash[PMAXIMUM];
      #endif
      #if HAS_Q_CENTER    /**SG**/
        backlash.distance_mm.q = (m.backlash[QMINIMUM] + m.backlash[QMAXIMUM]) / 2;
      #elif ENABLED(CALIBRATION_MEASURE_QMIN)
        backlash.distance_mm.q = m.backlash[QMINIMUM];
      #elif ENABLED(CALIBRATION_MEASURE_QMAX)
        backlash.distance_mm.q = m.backlash[QMAXIMUM];
      #endif

    #endif // BACKLASH_GCODE
  }

  #if ENABLED(BACKLASH_GCODE)
    // Turn on backlash compensation and move in all
    // allowed directions to take up any backlash
    {
      // New scope for TEMPORARY_BACKLASH_CORRECTION
      TEMPORARY_BACKLASH_CORRECTION(all_on);
      TEMPORARY_BACKLASH_SMOOTHING(0.0f);
      const xyz_float_t move = ARRAY_N(LINEAR_AXES,
        AXIS_CAN_CALIBRATE(X) * 3, AXIS_CAN_CALIBRATE(Y) * 3, AXIS_CAN_CALIBRATE(Z) * 3,
        AXIS_CAN_CALIBRATE(I) * 3, AXIS_CAN_CALIBRATE(J) * 3, AXIS_CAN_CALIBRATE(K) * 3,
        AXIS_CAN_CALIBRATE(M) * 3, AXIS_CAN_CALIBRATE(O) * 3, AXIS_CAN_CALIBRATE(P) * 3, AXIS_CAN_CALIBRATE(Q) * 3   /**SG**/
      );
      current_position += move; calibration_move();
      current_position -= move; calibration_move();
    }
  #endif
}

inline void update_measurements(measurements_t &m, const AxisEnum axis) {
  current_position[axis] += m.pos_error[axis];
  m.obj_center[axis] = true_center[axis];
  m.pos_error[axis] = 0;
}

/**
 * Probe around the calibration object. Adjust the position and toolhead offset
 * using the deviation from the known position of the calibration object.
 *
 *   m              in/out - Measurement record, updated with new readings
 *   uncertainty    in     - How far away from the object to begin probing
 *   extruder       in     - What extruder to probe
 *
 * Prerequisites:
 *    - Call calibrate_backlash() beforehand for best accuracy
 */
inline void calibrate_toolhead(measurements_t &m, const float uncertainty, const uint8_t extruder) {
  TEMPORARY_BACKLASH_CORRECTION(all_on);
  TEMPORARY_BACKLASH_SMOOTHING(0.0f);

  #if HAS_MULTI_HOTEND
    set_nozzle(m, extruder);
  #else
    UNUSED(extruder);
  #endif

  probe_sides(m, uncertainty);

  // Adjust the hotend offset
  #if HAS_HOTEND_OFFSET
    if (ENABLED(HAS_X_CENTER) && AXIS_CAN_CALIBRATE(X)) hotend_offset[extruder].x += m.pos_error.x;
    if (ENABLED(HAS_Y_CENTER) && AXIS_CAN_CALIBRATE(Y)) hotend_offset[extruder].y += m.pos_error.y;
                             if (AXIS_CAN_CALIBRATE(Z)) hotend_offset[extruder].z += m.pos_error.z;
    normalize_hotend_offsets();
  #endif

  // Correct for positional error, so the object
  // is at the known actual spot
  planner.synchronize();
  if (ENABLED(HAS_X_CENTER) && AXIS_CAN_CALIBRATE(X)) update_measurements(m, X_AXIS);
  if (ENABLED(HAS_Y_CENTER) && AXIS_CAN_CALIBRATE(Y)) update_measurements(m, Y_AXIS);
                           if (AXIS_CAN_CALIBRATE(Z)) update_measurements(m, Z_AXIS);

  TERN_(HAS_I_CENTER, update_measurements(m, I_AXIS));
  TERN_(HAS_J_CENTER, update_measurements(m, J_AXIS));
  TERN_(HAS_K_CENTER, update_measurements(m, K_AXIS));
  TERN_(HAS_M_CENTER, update_measurements(m, M_AXIS));  /**SG**/
  TERN_(HAS_O_CENTER, update_measurements(m, O_AXIS));  /**SG**/
  TERN_(HAS_P_CENTER, update_measurements(m, P_AXIS));  /**SG**/
  TERN_(HAS_Q_CENTER, update_measurements(m, Q_AXIS));  /**SG**/

  sync_plan_position();
}

/**
 * Probe around the calibration object for all toolheads, adjusting the coordinate
 * system for the first nozzle and the nozzle offset for subsequent nozzles.
 *
 *   m              in/out - Measurement record, updated with new readings
 *   uncertainty    in     - How far away from the object to begin probing
 */
inline void calibrate_all_toolheads(measurements_t &m, const float uncertainty) {
  TEMPORARY_BACKLASH_CORRECTION(all_on);
  TEMPORARY_BACKLASH_SMOOTHING(0.0f);

  HOTEND_LOOP() calibrate_toolhead(m, uncertainty, e);

  TERN_(HAS_HOTEND_OFFSET, normalize_hotend_offsets());

  TERN_(HAS_MULTI_HOTEND, set_nozzle(m, 0));
}

/**
 * Perform a full auto-calibration routine:
 *
 *   1) For each nozzle, touch top and sides of object to determine object position and
 *      nozzle offsets. Do a fast but rough search over a wider area.
 *   2) With the first nozzle, touch top and sides of object to determine backlash values
 *      for all axis (if BACKLASH_GCODE is enabled)
 *   3) For each nozzle, touch top and sides of object slowly to determine precise
 *      position of object. Adjust coordinate system and nozzle offsets so probed object
 *      location corresponds to known object location with a high degree of precision.
 */
inline void calibrate_all() {
  measurements_t m;

  TERN_(HAS_HOTEND_OFFSET, reset_hotend_offsets());

  TEMPORARY_BACKLASH_CORRECTION(all_on);
  TEMPORARY_BACKLASH_SMOOTHING(0.0f);

  // Do a fast and rough calibration of the toolheads
  calibrate_all_toolheads(m, CALIBRATION_MEASUREMENT_UNKNOWN);

  TERN_(BACKLASH_GCODE, calibrate_backlash(m, CALIBRATION_MEASUREMENT_UNCERTAIN));

  // Cycle the toolheads so the servos settle into their "natural" positions
  #if HAS_MULTI_HOTEND
    HOTEND_LOOP() set_nozzle(m, e);
  #endif

  // Do a slow and precise calibration of the toolheads
  calibrate_all_toolheads(m, CALIBRATION_MEASUREMENT_UNCERTAIN);

  current_position.x = X_CENTER;
  calibration_move();         // Park nozzle away from calibration object
}

/**
 * G425: Perform calibration with calibration object.
 *
 *   B           - Perform calibration of backlash only.
 *   T<extruder> - Perform calibration of toolhead only.
 *   V           - Probe object and print position, error, backlash and hotend offset.
 *   U           - Uncertainty, how far to start probe away from the object (mm)
 *
 *   no args     - Perform entire calibration sequence (backlash + position on all toolheads)
 */
void GcodeSuite::G425() {

  #ifdef CALIBRATION_SCRIPT_PRE
    GcodeSuite::process_subcommands_now_P(PSTR(CALIBRATION_SCRIPT_PRE));
  #endif

  if (homing_needed_error()) return;

  TEMPORARY_BED_LEVELING_STATE(false);
  SET_SOFT_ENDSTOP_LOOSE(true);

  measurements_t m;
  float uncertainty = parser.seenval('U') ? parser.value_float() : CALIBRATION_MEASUREMENT_UNCERTAIN;

  if (parser.seen('B'))
    calibrate_backlash(m, uncertainty);
  else if (parser.seen('T'))
    calibrate_toolhead(m, uncertainty, parser.has_value() ? parser.value_int() : active_extruder);
  #if ENABLED(CALIBRATION_REPORTING)
    else if (parser.seen('V')) {
      probe_sides(m, uncertainty);
      SERIAL_EOL();
      report_measured_faces(m);
      report_measured_center(m);
      report_measured_backlash(m);
      report_measured_nozzle_dimensions(m);
      report_measured_positional_error(m);
      #if HAS_HOTEND_OFFSET
        normalize_hotend_offsets();
        report_hotend_offsets();
      #endif
    }
  #endif
  else
    calibrate_all();

  SET_SOFT_ENDSTOP_LOOSE(false);

  #ifdef CALIBRATION_SCRIPT_POST
    GcodeSuite::process_subcommands_now_P(PSTR(CALIBRATION_SCRIPT_POST));
  #endif
}

#endif // CALIBRATION_GCODE
