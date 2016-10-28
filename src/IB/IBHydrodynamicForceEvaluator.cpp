// Filename: IBHydrodynamicForceEvaluator.cpp
// Created on 22 Oct 2016 by Amneet Bhalla
//
// Copyright (c) 2002-2014, Amneet Bhalla and Boyce Griffith
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of The University of North Carolina nor the names of
//      its contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/////////////////////////////// INCLUDES /////////////////////////////////////

#include "ArrayDataBasicOps.h"
#include "CartesianPatchGeometry.h"
#include "CellData.h"
#include "CoarseFineBoundary.h"
#include "PatchData.h"
#include "PatchHierarchy.h"
#include "SideData.h"
#include "SideIndex.h"
#include "boost/array.hpp"
#include "tbox/Pointer.h"
#include "tbox/RestartManager.h"
#include "ibamr/IBHydrodynamicForceEvaluator.h"
#include "ibamr/namespaces.h"
#include "ibtk/IndexUtilities.h"

/////////////////////////////// NAMESPACE ////////////////////////////////////

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////

/////////////////////////////// PUBLIC ///////////////////////////////////////

IBHydrodynamicForceEvaluator::IBHydrodynamicForceEvaluator(const std::string& object_name,
                                                           double rho,
                                                           double mu,
                                                           bool register_for_restart)
{
    d_object_name = object_name;
    d_rho = rho;
    d_mu = mu;

    VariableDatabase<NDIM>* var_db = VariableDatabase<NDIM>::getDatabase();
    Pointer<SideVariable<NDIM, double> > face_wgt_var = new SideVariable<NDIM, double>(d_object_name + "::face_wgt", 1);
    Pointer<VariableContext> face_wgt_ctx = var_db->getContext(d_object_name + "::face_wgt_ctx");
    d_face_wgt_sc_idx = var_db->registerVariableAndContext(face_wgt_var, face_wgt_ctx, /*ghost_width*/ 0);

    if (register_for_restart)
    {
        RestartManager::getManager()->registerRestartItem(d_object_name, this);
    }
    return;
} // IBHydrodynamicForceEvaluator

IBHydrodynamicForceEvaluator::~IBHydrodynamicForceEvaluator()
{
    VariableDatabase<NDIM>* var_db = VariableDatabase<NDIM>::getDatabase();
    var_db->removePatchDataIndex(d_face_wgt_sc_idx);

    return;
} // ~IBHydrodynamicForceEvaluator

void
IBHydrodynamicForceEvaluator::registerStructure(int strct_id,
                                                int strct_ln,
                                                const Eigen::Vector3d& box_vel,
                                                const Eigen::Vector3d& box_X_lower,
                                                const Eigen::Vector3d& box_X_upper)
{
#if !defined(NDEBUG)
    TBOX_ASSERT(d_hydro_objs.find(strct_id) == d_hydro_objs.end());
#endif

    IBHydrodynamicForceObject force_obj;
    force_obj.strct_id = strct_id;
    force_obj.strct_ln = strct_ln;

    bool from_restart = RestartManager::getManager()->isFromRestart();
    if (!from_restart)
    {
        force_obj.box_u_current = box_vel;
        force_obj.box_X_lower_current = box_X_lower;
        force_obj.box_X_upper_current = box_X_upper;
        force_obj.F_current.setZero();
        force_obj.T_current.setZero();
        force_obj.P_current.setZero();
        force_obj.L_current.setZero();
        force_obj.P_box_current.setZero();
        force_obj.L_box_current.setZero();
    }
    else
    {
        Pointer<Database> restart_db = RestartManager::getManager()->getRootDatabase();
        Pointer<Database> db;
        if (restart_db->isDatabase(d_object_name))
        {
            db = restart_db->getDatabase(d_object_name);
        }
        else
        {
            TBOX_ERROR("IBHydrodynamicForceEvaluator::registerStructure(). Restart database corresponding to "
                       << d_object_name
                       << " not found in restart file.\n");
        }

        std::ostringstream F, T, P, L, P_box, L_box, X_lo, X_hi;
        F << "F_" << strct_id;
        T << "T_" << strct_id;
        P << "P_" << strct_id;
        L << "L_" << strct_id;
        P_box << "P_box_" << strct_id;
        L_box << "L_box_" << strct_id;
        X_lo << "X_lo_" << strct_id;
        X_hi << "X_hi_" << strct_id;
        db->getDoubleArray(F.str(), force_obj.F_current.data(), 3);
        db->getDoubleArray(T.str(), force_obj.T_current.data(), 3);
        db->getDoubleArray(P.str(), force_obj.P_current.data(), 3);
        db->getDoubleArray(L.str(), force_obj.L_current.data(), 3);
        db->getDoubleArray(P_box.str(), force_obj.P_box_current.data(), 3);
        db->getDoubleArray(L_box.str(), force_obj.L_box_current.data(), 3);
        db->getDoubleArray(X_lo.str(), force_obj.box_X_lower_current.data(), 3);
        db->getDoubleArray(X_hi.str(), force_obj.box_X_upper_current.data(), 3);
    }

    d_hydro_objs[strct_id] = force_obj;
    return;

} // registerStructure

void
IBHydrodynamicForceEvaluator::updateStructureDomain(int strct_id,
                                                    int /*strct_ln*/,
                                                    double current_time,
                                                    double new_time,
                                                    const Eigen::Vector3d& box_vel_new,
                                                    const Eigen::Vector3d& P_strct_new,
                                                    const Eigen::Vector3d& L_strct_new)
{
#if !defined(NDEBUG)
    TBOX_ASSERT(d_hydro_objs.find(strct_id) != d_hydro_objs.end());
#endif

    IBHydrodynamicForceEvaluator::IBHydrodynamicForceObject& force_obj = d_hydro_objs[strct_id];
    const double dt = new_time - current_time;
    force_obj.box_u_new = box_vel_new;
    force_obj.box_X_lower_new = force_obj.box_X_lower_current + box_vel_new * dt;
    force_obj.box_X_upper_new = force_obj.box_X_upper_current + box_vel_new * dt;
    force_obj.P_new = P_strct_new;
    force_obj.L_new = L_strct_new;

    return;

} // updateStructureDomain

void
IBHydrodynamicForceEvaluator::preprocessIntegrateData(double /*current_time*/, double /*new_time*/)
{
    pout << "WARNING:: IBHydrodynamicForceEvaluator::preprocessIntegrateData() not implemented.\n";

    return;
} // preprocessIntegrateData

const IBHydrodynamicForceEvaluator::IBHydrodynamicForceObject&
IBHydrodynamicForceEvaluator::getHydrodynamicForceObject(int strct_id, int /*strct_ln*/)
{
#if !defined(NDEBUG)
    TBOX_ASSERT(d_hydro_objs.find(strct_id) != d_hydro_objs.end());
#endif

    const IBHydrodynamicForceEvaluator::IBHydrodynamicForceObject& force_obj = d_hydro_objs[strct_id];
    return force_obj;

} // getHydrodynamicForceObject

void
IBHydrodynamicForceEvaluator::computeHydrodynamicForce(int u_idx,
                                                       int p_idx,
                                                       int /*f_idx*/,
                                                       int vol_sc_idx,
                                                       int /*vol_cc_idx*/,
                                                       Pointer<PatchHierarchy<NDIM> > patch_hierarchy,
                                                       int coarsest_ln,
                                                       int finest_ln,
                                                       double current_time,
                                                       double new_time)
{
    computeFaceWeight(patch_hierarchy);
    const double dt = new_time - current_time;

    for (std::map<int, IBHydrodynamicForceObject>::iterator it = d_hydro_objs.begin(); it != d_hydro_objs.end(); ++it)
    {
        IBHydrodynamicForceObject& fobj = it->second;

        // Compute the momentum integral:= (rho * u * dv)
        fobj.P_box_new.setZero();
        for (int ln = finest_ln; ln >= coarsest_ln; --ln)
        {
            Pointer<PatchLevel<NDIM> > level = patch_hierarchy->getPatchLevel(ln);
            Box<NDIM> integration_box(
                IndexUtilities::getCellIndex(fobj.box_X_lower_new.data(), level->getGridGeometry(), level->getRatio()),
                IndexUtilities::getCellIndex(fobj.box_X_upper_new.data(), level->getGridGeometry(), level->getRatio()));
            for (PatchLevel<NDIM>::Iterator p(level); p; p++)
            {
                Pointer<Patch<NDIM> > patch = level->getPatch(p());
                const Box<NDIM>& patch_box = patch->getBox();
                const Pointer<CartesianPatchGeometry<NDIM> > patch_geom = patch->getPatchGeometry();
                const bool boxes_intersect = patch_box.intersects(integration_box);
                if (!boxes_intersect) continue;

                // Part of the box on this patch.
                Box<NDIM> trim_box = patch_box * integration_box;

                // Loop over the box and compute momemtum.
                Pointer<SideData<NDIM, double> > u_data = patch->getPatchData(u_idx);
                Pointer<SideData<NDIM, double> > vol_sc_data = patch->getPatchData(vol_sc_idx);
                for (Box<NDIM>::Iterator b(trim_box); b; b++)
                {
                    const CellIndex<NDIM>& cell_idx = *b;
                    for (int axis = 0; axis < NDIM; ++axis)
                    {
                        const SideIndex<NDIM> side_idx(cell_idx, axis, SideIndex<NDIM>::Lower);
                        const double& u_axis = (*u_data)(side_idx);
                        const double& vol = (*vol_sc_data)(side_idx);
                        fobj.P_box_new(axis) += d_rho * vol * u_axis;
                    }
                }
            }
        }
        SAMRAI_MPI::sumReduction(fobj.P_box_new.data(), 3);

        // Compute surface integral term.
        Eigen::Vector3d trac;
        trac.setZero();
        for (int ln = finest_ln; ln >= coarsest_ln; --ln)
        {
            Pointer<PatchLevel<NDIM> > level = patch_hierarchy->getPatchLevel(ln);
            Box<NDIM> integration_box(
                IndexUtilities::getCellIndex(fobj.box_X_lower_new.data(), level->getGridGeometry(), level->getRatio()),
                IndexUtilities::getCellIndex(fobj.box_X_upper_new.data(), level->getGridGeometry(), level->getRatio()));
            for (PatchLevel<NDIM>::Iterator p(level); p; p++)
            {
                Pointer<Patch<NDIM> > patch = level->getPatch(p());
                const Box<NDIM>& patch_box = patch->getBox();
                const Pointer<CartesianPatchGeometry<NDIM> > patch_geom = patch->getPatchGeometry();
                const double* const patch_dx = patch_geom->getDx();
                const bool boxes_intersect = patch_box.intersects(integration_box);
                if (!boxes_intersect) continue;

                // Store boxes corresponding to integration domain boundaries.
                boost::array<boost::array<Box<NDIM>, 2>, NDIM> bdry_boxes;
                for (int axis = 0; axis < NDIM; ++axis)
                {
                    Box<NDIM> bdry_box;

                    static const int lower_side = 0;
                    bdry_box = integration_box;
                    bdry_box.upper()(axis) = bdry_box.lower()(axis);
                    bdry_boxes[axis][lower_side] = bdry_box;

                    static const int upper_side = 1;
                    bdry_box = integration_box;
                    bdry_box.lower()(axis) = bdry_box.upper()(axis);
                    bdry_boxes[axis][upper_side] = bdry_box;
                }

                // Integrate over boundary boxes.
                Pointer<CellData<NDIM, double> > p_data = patch->getPatchData(p_idx);
                Pointer<SideData<NDIM, double> > u_data = patch->getPatchData(u_idx);
                Pointer<SideData<NDIM, double> > face_sc_data = patch->getPatchData(d_face_wgt_sc_idx);
                for (int axis = 0; axis < NDIM; ++axis)
                {
                    for (int upperlower = 0; upperlower <= 1; ++upperlower)
                    {
                        const Box<NDIM>& side_box = bdry_boxes[axis][upperlower];
                        if (!patch_box.intersects(side_box)) continue;

                        Box<NDIM> trim_box = patch_box * side_box;
                        Eigen::Vector3d n = Eigen::Vector3d::Zero();
                        n(axis) = upperlower ? 1 : -1;
                        for (Box<NDIM>::Iterator b(trim_box); b; b++)
                        {
                            const CellIndex<NDIM>& cell_idx = *b;
                            CellIndex<NDIM> cell_nbr_idx = cell_idx;
                            cell_nbr_idx(axis) += n(axis);
                            SideIndex<NDIM> bdry_idx(
                                cell_idx, axis, upperlower ? SideIndex<NDIM>::Upper : SideIndex<NDIM>::Lower);
                            const double& dA = (*face_sc_data)(bdry_idx);

                            // Pressure force := (n. -p I) * dA
                            trac += -0.5 * n * ((*p_data)(cell_idx) + (*p_data)(cell_nbr_idx)) * dA;

                            // Momentum force := (n. -rho*(u - u_b)u) * ds
                            Eigen::Vector3d u = Eigen::Vector3d::Zero();
                            for (int d = 0; d < NDIM; ++d)
                            {
                                if (d == axis)
                                {
                                    u(d) = (*u_data)(bdry_idx);
                                }
                                else
                                {
                                    CellIndex<NDIM> offset(0);
                                    offset(d) = 1;

                                    u(d) =
                                        0.25 *
                                        ((*u_data)(SideIndex<NDIM>(cell_idx, d, SideIndex<NDIM>::Lower)) +
                                         (*u_data)(SideIndex<NDIM>(cell_idx + offset, d, SideIndex<NDIM>::Lower)) +
                                         (*u_data)(SideIndex<NDIM>(cell_nbr_idx, d, SideIndex<NDIM>::Lower)) +
                                         (*u_data)(SideIndex<NDIM>(cell_nbr_idx + offset, d, SideIndex<NDIM>::Lower)));
                                }
                            }
                            trac += -d_rho * n.dot(u - fobj.box_u_new) * u * dA;

                            // Viscous traction force := n . mu(grad u + grad u ^ T) * ds
                            Eigen::Vector3d viscous_force = Eigen::Vector3d::Zero();
                            for (int d = 0; d < NDIM; ++d)
                            {
                                if (d == axis)
                                {
                                    viscous_force(axis) =
                                        n(axis) * (2.0 * d_mu) / (2.0 * patch_dx[axis]) *
                                        ((*u_data)(SideIndex<NDIM>(cell_nbr_idx, axis, SideIndex<NDIM>::Lower)) -
                                         (*u_data)(SideIndex<NDIM>(cell_idx, axis, SideIndex<NDIM>::Lower)));
                                }
                                else
                                {
                                    CellIndex<NDIM> offset(0);
                                    offset(d) = 1;

                                    viscous_force(d) =
                                        d_mu / (2.0 * patch_dx[d]) *
                                            ((*u_data)(
                                                 SideIndex<NDIM>(cell_idx + offset, axis, SideIndex<NDIM>::Lower)) -
                                             (*u_data)(
                                                 SideIndex<NDIM>(cell_idx - offset, axis, SideIndex<NDIM>::Lower)))

                                        +

                                        d_mu * n(axis) / (2.0 * patch_dx[axis]) *
                                            ((*u_data)(SideIndex<NDIM>(cell_nbr_idx, d, SideIndex<NDIM>::Lower)) +
                                             (*u_data)(
                                                 SideIndex<NDIM>(cell_nbr_idx + offset, d, SideIndex<NDIM>::Lower)) -
                                             (*u_data)(SideIndex<NDIM>(cell_idx, d, SideIndex<NDIM>::Lower)) -
                                             (*u_data)(SideIndex<NDIM>(cell_idx + offset, d, SideIndex<NDIM>::Lower))

                                                 );
                                }
                            }
                            trac += n(axis) * viscous_force * dA;
                        }
                    }
                }
            }
        }
        SAMRAI_MPI::sumReduction(trac.data(), 3);

        // Compute hydrodynamic force on the body : -d/dt(rho u)_box + d/dt(rho u)_body + trac
        fobj.F_new = (fobj.P_box_current - fobj.P_box_new + fobj.P_new - fobj.P_current) / dt + trac;
    }

    return;

} // computeHydrodynamicForce

void
IBHydrodynamicForceEvaluator::postprocessIntegrateData(double /*current_time*/, double /*new_time*/)
{
    for (std::map<int, IBHydrodynamicForceObject>::iterator it = d_hydro_objs.begin(); it != d_hydro_objs.end(); ++it)
    {
        IBHydrodynamicForceObject& force_obj = it->second;

        force_obj.box_u_current = force_obj.box_u_new;
        force_obj.box_X_lower_current = force_obj.box_X_lower_new;
        force_obj.box_X_upper_current = force_obj.box_X_upper_new;
        force_obj.F_current = force_obj.F_new;
        force_obj.T_current = force_obj.T_new;
        force_obj.P_current = force_obj.P_new;
        force_obj.L_current = force_obj.L_new;
        force_obj.P_box_current = force_obj.P_box_new;
        force_obj.L_box_current = force_obj.L_box_new;
    }

    return;

} // postprocessIntegrateData

void
IBHydrodynamicForceEvaluator::putToDatabase(SAMRAI::tbox::Pointer<SAMRAI::tbox::Database> db)
{
    for (std::map<int, IBHydrodynamicForceObject>::const_iterator it = d_hydro_objs.begin(); it != d_hydro_objs.end();
         ++it)
    {
        int strct_id = it->first;
        const IBHydrodynamicForceObject& force_obj = it->second;

        std::ostringstream F, T, P, L, P_box, L_box, X_lo, X_hi;
        F << "F_" << strct_id;
        T << "T_" << strct_id;
        P << "P_" << strct_id;
        L << "L_" << strct_id;
        P_box << "P_box_" << strct_id;
        L_box << "L_box_" << strct_id;
        X_lo << "X_lo_" << strct_id;
        X_hi << "X_hi_" << strct_id;

        db->putDoubleArray(F.str(), force_obj.F_current.data(), 3);
        db->putDoubleArray(T.str(), force_obj.T_current.data(), 3);
        db->putDoubleArray(P.str(), force_obj.P_current.data(), 3);
        db->putDoubleArray(L.str(), force_obj.L_current.data(), 3);
        db->putDoubleArray(P_box.str(), force_obj.P_box_current.data(), 3);
        db->putDoubleArray(L_box.str(), force_obj.L_box_current.data(), 3);
        db->putDoubleArray(X_lo.str(), force_obj.box_X_lower_current.data(), 3);
        db->putDoubleArray(X_hi.str(), force_obj.box_X_upper_current.data(), 3);
    }

    return;

} // putToDatabase

/////////////////////////////// PROTECTED ////////////////////////////////////

/////////////////////////////// PRIVATE //////////////////////////////////////

void
IBHydrodynamicForceEvaluator::computeFaceWeight(Pointer<PatchHierarchy<NDIM> > patch_hierarchy)
{
    const int coarsest_ln = 0;
    const int finest_ln = patch_hierarchy->getFinestLevelNumber();

    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = patch_hierarchy->getPatchLevel(ln);
        if (!level->checkAllocated(d_face_wgt_sc_idx)) level->allocatePatchData(d_face_wgt_sc_idx);
    }

    // Each cell's face weight is set to its face area, unless the cell is refined
    // on a finer level, in which case the weight is set to zero.  This insures
    // that no part of the physical domain is counted twice when discrete norms
    // and surface integrals are calculated on the entire hierarchy.
    //
    // Away from coarse-fine interfaces and boundaries of the computational
    // domain, each cell face's weight is set to the face area associated with
    // the level of the patch hierarchy.
    ArrayDataBasicOps<NDIM, double> array_ops;
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = patch_hierarchy->getPatchLevel(ln);
        BoxArray<NDIM> refined_region_boxes;
        if (ln < finest_ln)
        {
            Pointer<PatchLevel<NDIM> > next_finer_level = patch_hierarchy->getPatchLevel(ln + 1);
            refined_region_boxes = next_finer_level->getBoxes();
            refined_region_boxes.coarsen(next_finer_level->getRatioToCoarserLevel());
        }

        const IntVector<NDIM> max_gcw(1);
        const CoarseFineBoundary<NDIM> cf_bdry(*patch_hierarchy, ln, max_gcw);

        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            const Box<NDIM>& patch_box = patch->getBox();
            Pointer<CartesianPatchGeometry<NDIM> > pgeom = patch->getPatchGeometry();

            const double* const dx = pgeom->getDx();
            const double cell_vol = dx[0] * dx[1]
#if (NDIM > 2)
                                    * dx[2]
#endif
                ;
            Pointer<SideData<NDIM, double> > face_wgt_sc_data = patch->getPatchData(d_face_wgt_sc_idx);
            for (int axis = 0; axis < NDIM; ++axis)
            {
                ArrayData<NDIM, double>& axis_data = face_wgt_sc_data->getArrayData(axis);
                axis_data.fill(cell_vol / dx[axis]);
            }

            // Zero-out weights within the refined region.
            if (ln < finest_ln)
            {
                const IntVector<NDIM>& periodic_shift = level->getGridGeometry()->getPeriodicShift(level->getRatio());
                for (int i = 0; i < refined_region_boxes.getNumberOfBoxes(); ++i)
                {
                    for (unsigned int axis = 0; axis < NDIM; ++axis)
                    {
                        if (periodic_shift(axis) != 0)
                        {
                            for (int sgn = -1; sgn <= 1; sgn += 2)
                            {
                                IntVector<NDIM> periodic_offset = 0;
                                periodic_offset(axis) = sgn * periodic_shift(axis);
                                const Box<NDIM> refined_box =
                                    Box<NDIM>::shift(refined_region_boxes[i], periodic_offset);
                                const Box<NDIM> intersection = Box<NDIM>::grow(patch_box, 1) * refined_box;
                                if (!intersection.empty())
                                {
                                    face_wgt_sc_data->fillAll(0.0, intersection);
                                }
                            }
                        }
                    }
                    const Box<NDIM>& refined_box = refined_region_boxes[i];
                    const Box<NDIM> intersection = Box<NDIM>::grow(patch_box, 1) * refined_box;
                    if (!intersection.empty())
                    {
                        face_wgt_sc_data->fillAll(0.0, intersection);
                    }
                }
            }
        }
    }
    return;

} // computeFaceWeight

/////////////////////////////// NAMESPACE ////////////////////////////////////

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////
