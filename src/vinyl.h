#pragma once

// The record of a new track's intro, lit from the top left like the cassette.

#include "panel.h"

// a little smaller than the artwork, like a record in its sleeve; odd, so the center is a pixel
// and the spindle hole a single one
#define RECORD_SIZE 15

// the record with its left edge at x0, resting on the bottom row, its label in the album's
// color, turned turnDeg clockwise
void drawRecord(int x0, Rgb label, float turnDeg);
