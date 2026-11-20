/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019 Jean-Pierre Charras, jean-pierre.charras@ujf-grenoble.fr
 * Copyright (C) 2013 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 2013 Wayne Stambaugh <stambaughw@verizon.net>
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file spread_footprints.cpp
 * @brief Functions to spread footprints on free areas outside a board without overlapping text.
 */

#include "spread_footprints.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

#include <footprint.h>
#include <pcb_edit_frame.h>
#include <rectpack2d/finders_interface.h>
#include <refdes_utils.h>
#include <string_utils.h>


constexpr bool allow_flip = true;

using SPACES_T = rectpack2D::empty_spaces<allow_flip, rectpack2D::default_empty_spaces>;
using RECT_T   = rectpack2D::output_rect_t<SPACES_T>;

// Use 0.01 mm units to calculate placement, to avoid long calculation time
const int scale = (int) ( 0.01 * pcbIUScale.IU_PER_MM );


namespace
{

struct SPREAD_GROUP_KEY
{
    int bodyWBin  = 0;
    int bodyHBin  = 0;
    int fullWTier = 0;
    int fullHTier = 0;

    bool operator<( const SPREAD_GROUP_KEY& aOther ) const
    {
        return std::tie( bodyWBin, bodyHBin, fullWTier, fullHTier )
               < std::tie( aOther.bodyWBin, aOther.bodyHBin, aOther.fullWTier, aOther.fullHTier );
    }
};


static VECTOR2I getBoxCenterSafe( const BOX2I& aBox )
{
    return VECTOR2I( static_cast<int>( static_cast<int64_t>( aBox.GetX() ) + static_cast<int64_t>( aBox.GetWidth() ) / 2 ),
                     static_cast<int>( static_cast<int64_t>( aBox.GetY() ) + static_cast<int64_t>( aBox.GetHeight() ) / 2 ) );
}


/**
 * 获取包含焊盘、丝印框及可见文字（位号 Reference / 数值 Value 等）的完整防撞包围盒
 */
static BOX2I getFootprintSafeBBox( const FOOTPRINT* aFootprint )
{
    BOX2I bbox     = aFootprint->GetBoundingBox( false );
    BOX2I textBBox = aFootprint->GetBoundingBox( true );

    if( textBBox.GetWidth() > 0 && textBBox.GetHeight() > 0 )
    {
        if( bbox.GetWidth() > 0 || bbox.GetHeight() > 0 )
            bbox.Merge( textBBox );
        else
            bbox = textBBox;
    }

    return bbox;
}

} // namespace


static bool compareFootprintsbyRef( FOOTPRINT* ref, FOOTPRINT* compare )
{
    const wxString& refPrefix = UTIL::GetRefDesPrefix( ref->GetReference() );
    const wxString& cmpPrefix = UTIL::GetRefDesPrefix( compare->GetReference() );

    if( refPrefix != cmpPrefix )
    {
        return refPrefix < cmpPrefix;
    }
    else
    {
        const int refInt = GetTrailingInt( ref->GetReference() );
        const int cmpInt = GetTrailingInt( compare->GetReference() );

        return refInt < cmpInt;
    }
}


// Spread a list of rectangles inside a placement area
std::optional<rectpack2D::rect_wh> spreadRectangles( std::vector<RECT_T>& vecSubRects, int areaSizeX, int areaSizeY )
{
    areaSizeX /= scale;
    areaSizeY /= scale;

    std::optional<rectpack2D::rect_wh> result;

    int max_side = std::max<int>( 10, std::max<int>( areaSizeX, areaSizeY ) );

    for( int i = 0; i < 2000; i++ )
    {
        bool      anyUnsuccessful = false;
        const int discard_step    = 1;

        auto report_successful =
                [&]( RECT_T& )
                {
                    return rectpack2D::callback_result::CONTINUE_PACKING;
                };

        auto report_unsuccessful =
                [&]( RECT_T& )
                {
                    anyUnsuccessful = true;
                    return rectpack2D::callback_result::ABORT_PACKING;
                };

        result = rectpack2D::find_best_packing<SPACES_T>( vecSubRects,
                                                          make_finder_input( max_side, discard_step,
                                                                             report_successful, report_unsuccessful,
                                                                             rectpack2D::flipping_option::DISABLED ) );

        if( !result || anyUnsuccessful )
        {
            max_side = std::max<int>( max_side + 10, static_cast<int>( max_side * 1.2 ) );
            continue;
        }

        break;
    }

    return result;
}


void SpreadFootprints( std::vector<FOOTPRINT*>* aFootprints, const VECTOR2I& aTargetBoxPosition, bool aGroupBySheet,
                       int aComponentGap, int aGroupGap )
{
    if( !aFootprints || aFootprints->empty() )
        return;

    // 保障元件及文字外沿至少留有 1.0mm 安全净空，组块之间至少留有 2.2mm 间隔，彻底杜绝压字
    const int effectiveCompGap  = std::max<int>( aComponentGap, static_cast<int>( pcbIUScale.mmToIU( 1.0 ) ) );
    const int effectiveBlockGap = std::max<int>( aComponentGap * 2, static_cast<int>( pcbIUScale.mmToIU( 2.2 ) ) );
    const int effectiveGroupGap = std::max<int>( aGroupGap, static_cast<int>( pcbIUScale.mmToIU( 3.0 ) ) );

    const int bodyQuantStep = std::max<int>( 1, static_cast<int>( pcbIUScale.mmToIU( 0.2 ) ) );
    const int textWTierStep = std::max<int>( 1, static_cast<int>( pcbIUScale.mmToIU( 4.0 ) ) );
    const int textHTierStep = std::max<int>( 1, static_cast<int>( pcbIUScale.mmToIU( 3.0 ) ) );

    using FpBBoxToFootprintsPair       = std::pair<BOX2I, std::vector<FOOTPRINT*>>;
    using SheetBBoxToFootprintsMapPair = std::pair<BOX2I, std::map<SPREAD_GROUP_KEY, FpBBoxToFootprintsPair>>;

    std::map<wxString, SheetBBoxToFootprintsMapPair> sheetsMap;

    // 1. 按同封装尺寸 + 文字跨度分档归组
    for( FOOTPRINT* footprint : *aFootprints )
    {
        if( !footprint )
            continue;

        wxString path = aGroupBySheet ? footprint->GetPath().AsString().BeforeLast( '/' ) : wxString( wxS( "" ) );

        BOX2I bodyBox = footprint->GetBoundingBox( false );
        BOX2I fullBox = getFootprintSafeBBox( footprint );

        SPREAD_GROUP_KEY key;
        key.bodyWBin  = static_cast<int>( ( static_cast<int64_t>( bodyBox.GetWidth() ) + bodyQuantStep / 2 ) / bodyQuantStep );
        key.bodyHBin  = static_cast<int>( ( static_cast<int64_t>( bodyBox.GetHeight() ) + bodyQuantStep / 2 ) / bodyQuantStep );
        key.fullWTier = static_cast<int>( fullBox.GetWidth() / textWTierStep );
        key.fullHTier = static_cast<int>( fullBox.GetHeight() / textHTierStep );

        sheetsMap[path].second[key].second.push_back( footprint );
    }

    for( auto& [sheetPath, sheetPair] : sheetsMap )
    {
        auto& [sheet_bbox, groupToFpMap] = sheetPair;

        for( auto& [groupKey, fpPair] : groupToFpMap )
        {
            auto& [block_bbox, footprints] = fpPair;

            if( footprints.empty() )
                continue;

            std::sort( footprints.begin(), footprints.end(), compareFootprintsbyRef );

            // 统计组内所有元件相对于各自本体中心的左/右/上/下最大文字外伸量
            int maxLeftExt   = 0;
            int maxRightExt  = 0;
            int maxTopExt    = 0;
            int maxBottomExt = 0;

            for( const FOOTPRINT* fp : footprints )
            {
                BOX2I    bodyBox = fp->GetBoundingBox( false );
                BOX2I    fullBox = getFootprintSafeBBox( fp );
                VECTOR2I center  = ( bodyBox.GetWidth() > 0 || bodyBox.GetHeight() > 0 )
                                           ? getBoxCenterSafe( bodyBox )
                                           : getBoxCenterSafe( fullBox );

                maxLeftExt   = std::max<int>( maxLeftExt, std::max<int>( 0, static_cast<int>( center.x - fullBox.GetLeft() ) ) );
                maxRightExt  = std::max<int>( maxRightExt, std::max<int>( 0, static_cast<int>( fullBox.GetRight() - center.x ) ) );
                maxTopExt    = std::max<int>( maxTopExt, std::max<int>( 0, static_cast<int>( center.y - fullBox.GetTop() ) ) );
                maxBottomExt = std::max<int>( maxBottomExt, std::max<int>( 0, static_cast<int>( fullBox.GetBottom() - center.y ) ) );
            }

            VECTOR2I cellPitch( maxLeftExt + maxRightExt + effectiveCompGap,
                                maxTopExt + maxBottomExt + effectiveCompGap );

            cellPitch.x = std::max<int>( cellPitch.x, static_cast<int>( pcbIUScale.mmToIU( 1.5 ) ) );
            cellPitch.y = std::max<int>( cellPitch.y, static_cast<int>( pcbIUScale.mmToIU( 1.5 ) ) );

            // 计算行列折行比例
            double       blockEstimateArea   = static_cast<double>( cellPitch.x ) * static_cast<double>( cellPitch.y ) * static_cast<double>( footprints.size() );
            double       initialSide         = std::sqrt( blockEstimateArea );
            bool         vertical            = cellPitch.x >= cellPitch.y;
            int          initialCountPerLine = static_cast<int>( footprints.size() );
            const double singleLineRatio     = 5.0;

            if( footprints.size() > 1 )
            {
                if( vertical )
                {
                    if( ( static_cast<double>( cellPitch.y ) * static_cast<double>( footprints.size() ) / static_cast<double>( cellPitch.x ) ) > singleLineRatio )
                        initialCountPerLine = std::max<int>( 1, static_cast<int>( KiROUND( initialSide / static_cast<double>( cellPitch.y ) ) ) );
                }
                else
                {
                    if( ( static_cast<double>( cellPitch.x ) * static_cast<double>( footprints.size() ) / static_cast<double>( cellPitch.y ) ) > singleLineRatio )
                        initialCountPerLine = std::max<int>( 1, static_cast<int>( KiROUND( initialSide / static_cast<double>( cellPitch.x ) ) ) );
                }
            }

            initialCountPerLine = std::clamp<int>( initialCountPerLine, 1, static_cast<int>( footprints.size() ) );

            int optimalCountPerLine = initialCountPerLine;
            int optimalRemainder    = static_cast<int>( footprints.size() ) % optimalCountPerLine;

            if( optimalRemainder != 0 && footprints.size() >= 4 )
            {
                for( int i = std::max<int>( 2, initialCountPerLine - 2 );
                     i <= std::min<int>( static_cast<int>( footprints.size() ) - 2, initialCountPerLine + 2 ); i++ )
                {
                    int r = static_cast<int>( footprints.size() ) % i;

                    if( r == 0 || r >= optimalRemainder )
                    {
                        optimalCountPerLine = i;
                        optimalRemainder    = r;

                        if( r == 0 )
                            break;
                    }
                }
            }

            // 2. 将组内每个元件的焊盘几何中心对齐到等距网格，确保文字 100% 落在安全格子内
            for( unsigned i = 0; i < footprints.size(); i++ )
            {
                FOOTPRINT* footprint = footprints[i];

                int col = vertical ? ( static_cast<int>( i ) / optimalCountPerLine ) : ( static_cast<int>( i ) % optimalCountPerLine );
                int row = vertical ? ( static_cast<int>( i ) % optimalCountPerLine ) : ( static_cast<int>( i ) / optimalCountPerLine );

                VECTOR2I targetBodyCenter( col * cellPitch.x + effectiveCompGap / 2 + maxLeftExt,
                                           row * cellPitch.y + effectiveCompGap / 2 + maxTopExt );

                BOX2I    oldBodyBox = footprint->GetBoundingBox( false );
                BOX2I    oldFullBox = getFootprintSafeBBox( footprint );
                VECTOR2I curCenter  = ( oldBodyBox.GetWidth() > 0 || oldBodyBox.GetHeight() > 0 )
                                              ? getBoxCenterSafe( oldBodyBox )
                                              : getBoxCenterSafe( oldFullBox );

                footprint->Move( targetBodyCenter - curCenter );

                BOX2I placedFullBox = getFootprintSafeBBox( footprint );
                placedFullBox.Inflate( effectiveCompGap / 2 );

                if( i == 0 )
                    block_bbox = placedFullBox;
                else
                    block_bbox.Merge( placedFullBox );
            }

            // 为每个组块外围增加组间留白，防止不同类型器件边缘文字相碰
            block_bbox.Inflate( effectiveBlockGap / 2 );
        }

        std::vector<RECT_T> vecSubRects;
        double              blocksArea = 0.0;

        // 3. 向上取整转换组块尺寸并进行二维装箱排布
        for( auto& [groupKey, fpPair] : groupToFpMap )
        {
            auto& [block_bbox, footprints] = fpPair;

            int rectW = std::max<int>( 1, static_cast<int>( ( static_cast<int64_t>( block_bbox.GetWidth() ) + scale - 1 ) / scale ) );
            int rectH = std::max<int>( 1, static_cast<int>( ( static_cast<int64_t>( block_bbox.GetHeight() ) + scale - 1 ) / scale ) );

            vecSubRects.emplace_back( 0, 0, rectW, rectH, false );
            blocksArea += static_cast<double>( block_bbox.GetWidth() ) * static_cast<double>( block_bbox.GetHeight() );
        }

        int areaSide = std::max<int>( scale * 10, static_cast<int>( KiROUND( std::sqrt( blocksArea ) ) ) );
        spreadRectangles( vecSubRects, areaSide, areaSide );

        unsigned block_i      = 0;
        bool     firstInSheet = true;

        for( auto& [groupKey, pair] : groupToFpMap )
        {
            auto& [src_bbox, footprints] = pair;

            RECT_T srect = vecSubRects[block_i];

            VECTOR2I target_pos( srect.x * scale, srect.y * scale );
            VECTOR2I target_size( srect.w * scale, srect.h * scale );

            if( static_cast<uint64_t>( target_pos.x ) + static_cast<uint64_t>( target_size.x ) > INT_MAX / 2 )
                target_pos.x -= INT_MAX / 2;

            if( static_cast<uint64_t>( target_pos.y ) + static_cast<uint64_t>( target_size.y ) > INT_MAX / 2 )
                target_pos.y -= INT_MAX / 2;

            VECTOR2I delta = target_pos - src_bbox.GetPosition();

            for( FOOTPRINT* footprint : footprints )
            {
                footprint->Move( delta );

                BOX2I fpFullBox = getFootprintSafeBBox( footprint );
                if( firstInSheet )
                {
                    sheet_bbox   = fpFullBox;
                    firstInSheet = false;
                }
                else
                {
                    sheet_bbox.Merge( fpFullBox );
                }
            }

            block_i++;
        }
    }

    std::vector<RECT_T> vecSubRects;
    double              sheetsArea = 0.0;

    // 4. 对多页原理图分组打包并保留页间间隔
    for( auto& [sheetPath, sheetPair] : sheetsMap )
    {
        auto& [sheet_bbox, groupToFpMap] = sheetPair;

        sheet_bbox.Inflate( effectiveGroupGap / 2 );

        int rectW = std::max<int>( 1, static_cast<int>( ( static_cast<int64_t>( sheet_bbox.GetWidth() ) + scale - 1 ) / scale ) );
        int rectH = std::max<int>( 1, static_cast<int>( ( static_cast<int64_t>( sheet_bbox.GetHeight() ) + scale - 1 ) / scale ) );

        vecSubRects.emplace_back( 0, 0, rectW, rectH, false );
        sheetsArea += static_cast<double>( sheet_bbox.GetWidth() ) * static_cast<double>( sheet_bbox.GetHeight() );
    }

    int areaSide = std::max<int>( scale * 10, static_cast<int>( KiROUND( std::sqrt( sheetsArea ) ) ) );
    spreadRectangles( vecSubRects, areaSide, areaSide );

    unsigned srect_i = 0;

    for( auto& [sheetPath, sheetPair] : sheetsMap )
    {
        auto& [src_bbox, groupToFpMap] = sheetPair;

        RECT_T srect = vecSubRects[srect_i];

        VECTOR2I target_pos( srect.x * scale + aTargetBoxPosition.x,
                             srect.y * scale + aTargetBoxPosition.y );
        VECTOR2I target_size( srect.w * scale, srect.h * scale );

        if( static_cast<int64_t>( target_pos.x ) + static_cast<int64_t>( target_size.x ) > INT_MAX / 2 )
            target_pos.x -= INT_MAX / 2;

        if( static_cast<int64_t>( target_pos.y ) + static_cast<int64_t>( target_size.y ) > INT_MAX / 2 )
            target_pos.y -= INT_MAX / 2;

        VECTOR2I delta = target_pos - src_bbox.GetPosition();

        for( auto& [groupKey, fpPair] : groupToFpMap )
        {
            auto& [block_bbox, footprints] = fpPair;

            for( FOOTPRINT* footprint : footprints )
                footprint->Move( delta );
        }

        srect_i++;
    }
}