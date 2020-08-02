/*
 * virmigration.h: hypervisor migration handling
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "virenum.h"


typedef enum {
    VIR_MIGRATION_PHASE_NONE = 0,
    VIR_MIGRATION_PHASE_PERFORM2,
    VIR_MIGRATION_PHASE_BEGIN3,
    VIR_MIGRATION_PHASE_PERFORM3,
    VIR_MIGRATION_PHASE_PERFORM3_DONE,
    VIR_MIGRATION_PHASE_CONFIRM3_CANCELLED,
    VIR_MIGRATION_PHASE_CONFIRM3,
    VIR_MIGRATION_PHASE_PREPARE,
    VIR_MIGRATION_PHASE_FINISH2,
    VIR_MIGRATION_PHASE_FINISH3,

    VIR_MIGRATION_PHASE_LAST
} virMigrationJobPhase;
VIR_ENUM_DECL(virMigrationJobPhase);
