#pragma once

#include <d3d11.h>

#include "log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);

/* lives in main.cpp */
extern Log log;

}
