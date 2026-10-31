#pragma once

// The record of a new track's intro, lit from the top left like the cassette.

#include "panel.h"

#define RECORD_SIZE 16  // the record fills a square this wide, as the artwork does

// the record with its left edge at x0, its label in the album's color, turned turnDeg
// clockwise; level fades it
void drawRecord(int x0, Rgb label, float turnDeg, float level);
