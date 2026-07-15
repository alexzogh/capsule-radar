#pragma once
// Clock face / idle mode overlay. Shown on lv_layer_top() when the device
// has been idle (no touch) for the configured period. Touch or shake to wake.
// The radar keeps polling underneath — this is purely a visual overlay.

void clock_create();                // create the clock widgets (call once, after ui_create)
void clock_update(int aircraftCount, const char *nearestCall,
                  float nearestDistKm, float nearestAltFt);
void clock_show();                  // make visible, start 1 s update timer
void clock_hide();                  // hide, stop the timer
bool clock_visible();
