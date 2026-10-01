#pragma once
#include <RE/N/NiTransform.h>
namespace direct_render {
void install();
bool available();
void publish(bool active, const RE::NiTransform& car, const RE::NiTransform& ball, bool car_visible, bool ball_visible, bool boosting=false);
}
