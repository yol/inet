//
// Copyright (C) 2013 OpenSim Ltd.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#ifndef __INET_SIMPLEBATTERY_H
#define __INET_SIMPLEBATTERY_H

#include "IPowerAccumulator.h"
#include "PowerSourceBase.h"
#include "PowerSinkBase.h"

namespace inet {

namespace power {

/**
 * This class implements a simple battery. The simple battery is determined by
 * its nominal capacity.
 *
 * @author Levente Meszaros
 */
class INET_API SimpleBattery : public PowerSourceBase, public PowerSinkBase, public virtual IPowerAccumulator
{
  protected:
    /**
     * Specifies if the node should be crashed when the battery becomes depleted.
     */
    bool crashNodeWhenDepleted;

    /**
     * Nominal capacity.
     */
    J nominalCapacity;

    /**
     * Residual capacity.
     */
    J residualCapacity;

    /**
     * Last simulation time when residual capacity was updated.
     */
    simtime_t lastResidualCapacityUpdate;

    /**
     * Timer that is scheduled to the time when the battery will be depleted.
     */
    cMessage *depletedTimer;

  public:
    SimpleBattery();

    virtual ~SimpleBattery();

    virtual J getNominalCapacity() { return nominalCapacity; }

    virtual J getResidualCapacity() { updateResidualCapacity(); return residualCapacity; }

    virtual void setPowerConsumption(int id, W consumedPower);

  protected:
    virtual void initialize(int stage);

    virtual void handleMessage(cMessage *message);

    virtual void updateResidualCapacity();

    virtual void scheduleDepletedTimer();
};

} // namespace power

} // namespace inet

#endif // ifndef __INET_SIMPLEBATTERY_H
