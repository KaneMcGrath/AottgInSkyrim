// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace Aottg::Render {
    // Skyrim publishes a row-vector perspective projection (clip W = view Z).
    template<class Matrix> constexpr bool ReverseProjection(const Matrix& projection) {
        return projection.m[2][2]*projection.m[2][3]<0;
    }
    inline void MatchDepthConvention(float (&matrix)[4][4],bool sourceReversed,bool targetReversed) {
        if (sourceReversed!=targetReversed)
            for (int col=0;col<4;++col) matrix[2][col]=matrix[3][col]-matrix[2][col];
    }
}
