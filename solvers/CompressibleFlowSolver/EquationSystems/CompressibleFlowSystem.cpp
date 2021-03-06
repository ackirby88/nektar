///////////////////////////////////////////////////////////////////////////////
//
// File CompressibleFlowSystem.cpp
//
// For more information, please see: http://www.nektar.info
//
// The MIT License
//
// Copyright (c) 2006 Division of Applied Mathematics, Brown University (USA),
// Department of Aeronautics, Imperial College London (UK), and Scientific
// Computing and Imaging Institute, University of Utah (USA).
//
// License for the specific language governing rights and limitations under
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
// Description: Auxiliary functions for the compressible flow system
//
///////////////////////////////////////////////////////////////////////////////

#include <CompressibleFlowSolver/EquationSystems/CompressibleFlowSystem.h>
#include <LocalRegions/TriExp.h>
#include <LocalRegions/QuadExp.h>
#include <LocalRegions/HexExp.h>
#include <MultiRegions/ExpList.h>
#include <MultiRegions/AssemblyMap/AssemblyMapDG.h>

namespace Nektar
{
    string CompressibleFlowSystem::className =
        SolverUtils::GetEquationSystemFactory().RegisterCreatorFunction(
            "CompressibleFlowSystem",
            CompressibleFlowSystem::create,
            "Auxiliary functions for the compressible flow system.");

    CompressibleFlowSystem::CompressibleFlowSystem(
        const LibUtilities::SessionReaderSharedPtr& pSession)
        : UnsteadySystem(pSession)
    {
    }

    /**
     * @brief Initialization object for CompressibleFlowSystem class.
     */
    void CompressibleFlowSystem::v_InitObject()
    {
        UnsteadySystem::v_InitObject();

        ASSERTL0(m_session->DefinesSolverInfo("UPWINDTYPE"),
                 "No UPWINDTYPE defined in session.");

        // Do not forwards transform initial condition
        m_homoInitialFwd = false;

        // Set up locations of velocity vector.
        m_velLoc = Array<OneD, NekDouble>(m_spacedim);
        for (int i = 0; i < m_spacedim; ++i)
        {
            m_velLoc[i] = i+1;
        }

        // Get gamma parameter from session file.
        ASSERTL0(m_session->DefinesParameter("Gamma"),
                 "Compressible flow sessions must define a Gamma parameter.");
        m_session->LoadParameter("Gamma", m_gamma, 1.4);

        // Get E0 parameter from session file.
        ASSERTL0(m_session->DefinesParameter("pInf"),
                 "Compressible flow sessions must define a pInf parameter.");
        m_session->LoadParameter("pInf", m_pInf, 101325);

        // Get rhoInf parameter from session file.
        ASSERTL0(m_session->DefinesParameter("rhoInf"),
                 "Compressible flow sessions must define a rhoInf parameter.");
        m_session->LoadParameter("rhoInf", m_rhoInf, 1.225);

        // Get uInf parameter from session file.
        ASSERTL0(m_session->DefinesParameter("uInf"),
                 "Compressible flow sessions must define a uInf parameter.");
        m_session->LoadParameter("uInf", m_uInf, 0.1);

        // Get vInf parameter from session file.
        if (m_spacedim == 2 || m_spacedim == 3)
        {
            ASSERTL0(m_session->DefinesParameter("vInf"),
                     "Compressible flow sessions must define a vInf parameter"
                     "for 2D/3D problems.");
            m_session->LoadParameter("vInf", m_vInf, 0.0);
        }

        // Get wInf parameter from session file.
        if (m_spacedim == 3)
        {
            ASSERTL0(m_session->DefinesParameter("wInf"),
                     "Compressible flow sessions must define a wInf parameter"
                     "for 3D problems.");
            m_session->LoadParameter("wInf", m_wInf, 0.0);
        }

        m_session->LoadParameter ("GasConstant",   m_gasConstant,   287.058);
        m_session->LoadParameter ("Twall",         m_Twall,         300.15);
        m_session->LoadSolverInfo("ViscosityType", m_ViscosityType, "Constant");
        m_session->LoadParameter ("mu",            m_mu,            1.78e-05);
        m_session->LoadParameter ("thermalConductivity",
                                  m_thermalConductivity, 0.0257);

        m_Cp      = m_gamma / (m_gamma - 1.0) * m_gasConstant;
        m_Prandtl = m_Cp * m_mu / m_thermalConductivity;

        // Type of advection class to be used
        switch(m_projectionType)
        {
            // Continuous field
            case MultiRegions::eGalerkin:
            {
                ASSERTL0(false, "Continuous field not supported.");
                break;
            }
            // Discontinuous field
            case MultiRegions::eDiscontinuous:
            {
                string advName, diffName, riemName;

                // Setting up advection and diffusion operators
                m_session->LoadSolverInfo("AdvectionType", advName, "WeakDG");
                m_session->LoadSolverInfo("DiffusionType", diffName, "LDGNS");
                m_advection = SolverUtils::GetAdvectionFactory()
                                            .CreateInstance(advName, advName);
                m_diffusion = SolverUtils::GetDiffusionFactory()
                                            .CreateInstance(diffName, diffName);

                // Setting up flux vector for advection operator
                if (m_specHP_dealiasing)
                {
                    m_advection->SetFluxVector(&CompressibleFlowSystem::
                                               GetFluxVectorDeAlias, this);
                }
                else
                {
                    m_advection->SetFluxVector(&CompressibleFlowSystem::
                                               GetFluxVector, this);
                }

                // Setting up flux vector for diffusion operator
                if (m_specHP_dealiasing)
                {
                    m_diffusion->SetFluxVectorNS(
                                            &CompressibleFlowSystem::
                                            GetViscousFluxVectorDeAlias, this);
                }
                else
                {
                    m_diffusion->SetFluxVectorNS(&CompressibleFlowSystem::
                                                 GetViscousFluxVector, this);
                }

                // Setting up Riemann solver for advection operator
                m_session->LoadSolverInfo("UpwindType", riemName, "Average");
                m_riemannSolver = SolverUtils::GetRiemannSolverFactory()
                                            .CreateInstance(riemName);

                // Setting up upwind solver for diffusion operator
                m_riemannSolverLDG = SolverUtils::GetRiemannSolverFactory()
                                                .CreateInstance("UpwindLDG");

                // Setting up parameters for advection operator Riemann solver
                m_riemannSolver->SetParam (
                    "gamma",  &CompressibleFlowSystem::GetGamma,   this);
                m_riemannSolver->SetAuxiliary(
                    "velLoc", &CompressibleFlowSystem::GetVelLoc,  this);
                m_riemannSolver->SetVector(
                    "N",      &CompressibleFlowSystem::GetNormals, this);

                // Setting up parameters for diffusion operator Riemann solver
                m_riemannSolverLDG->SetParam (
                    "gamma",  &CompressibleFlowSystem::GetGamma,   this);
                m_riemannSolverLDG->SetAuxiliary(
                    "velLoc", &CompressibleFlowSystem::GetVelLoc,  this);
                m_riemannSolverLDG->SetVector(
                    "N",      &CompressibleFlowSystem::GetNormals, this);

                // Concluding initialisation of advection / diffusion operators
                m_advection->SetRiemannSolver   (m_riemannSolver);
                m_diffusion->SetRiemannSolver   (m_riemannSolverLDG);
                m_advection->InitObject         (m_session, m_fields);
                m_diffusion->InitObject         (m_session, m_fields);
                break;
            }
            default:
            {
                ASSERTL0(false, "Unsupported projection type.");
                break;
            }
        }
    }

    /**
     * @brief Destructor for CompressibleFlowSystem class.
     */
    CompressibleFlowSystem::~CompressibleFlowSystem()
    {

    }

    /**
     * @brief Print out a summary with some relevant information.
     */
    void CompressibleFlowSystem::v_GenerateSummary(SolverUtils::SummaryList& s)
    {
        UnsteadySystem::v_GenerateSummary(s);
    }

    /**
     * @brief Wall boundary conditions for compressible flow problems.
     */
    void CompressibleFlowSystem::WallBC(
        int                                   bcRegion,
        int                                   cnt,
        Array<OneD, Array<OneD, NekDouble> > &physarray)
    {
        int i;
        int nTracePts = GetTraceTotPoints();
        int nVariables = physarray.num_elements();

        const Array<OneD, const int> &traceBndMap
            = m_fields[0]->GetTraceBndMap();

        // Get physical values of the forward trace
        Array<OneD, Array<OneD, NekDouble> > Fwd(nVariables);
        for (i = 0; i < nVariables; ++i)
        {
            Fwd[i] = Array<OneD, NekDouble>(nTracePts);
            m_fields[i]->ExtractTracePhys(physarray[i], Fwd[i]);
        }

        // Adjust the physical values of the trace to take
        // user defined boundaries into account
        int e, id1, id2, nBCEdgePts, eMax;

        eMax = m_fields[0]->GetBndCondExpansions()[bcRegion]->GetExpSize();

        for (e = 0; e < eMax; ++e)
        {
            nBCEdgePts = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetExp(e)->GetTotPoints();
            id1 = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetPhys_Offset(e);
            id2 = m_fields[0]->GetTrace()->GetPhys_Offset(traceBndMap[cnt+e]);

            // For 2D/3D, define: v* = v - 2(v.n)n
            Array<OneD, NekDouble> tmp(nBCEdgePts, 0.0);

            // Calculate (v.n)
            for (i = 0; i < m_spacedim; ++i)
            {
                Vmath::Vvtvp(nBCEdgePts,
                             &Fwd[1+i][id2], 1,
                             &m_traceNormals[i][id2], 1,
                             &tmp[0], 1,
                             &tmp[0], 1);
            }

            // Calculate 2.0(v.n)
            Vmath::Smul(nBCEdgePts, -2.0, &tmp[0], 1, &tmp[0], 1);

            // Calculate v* = v - 2.0(v.n)n
            for (i = 0; i < m_spacedim; ++i)
            {
                Vmath::Vvtvp(nBCEdgePts,
                             &tmp[0], 1,
                             &m_traceNormals[i][id2], 1,
                             &Fwd[1+i][id2], 1,
                             &Fwd[1+i][id2], 1);
            }

            // Copy boundary adjusted values into the boundary expansion
            for (i = 0; i < nVariables; ++i)
            {
                Vmath::Vcopy(nBCEdgePts, &Fwd[i][id2], 1,
                             &(m_fields[i]->GetBndCondExpansions()[bcRegion]->
                             UpdatePhys())[id1], 1);
            }
        }
    }

    /**
     * @brief Wall boundary conditions for viscous compressible flow problems.
     */
    void CompressibleFlowSystem::WallViscousBC(
        int                                   bcRegion,
        int                                   cnt,
        Array<OneD, Array<OneD, NekDouble> > &physarray)
    {
        int i;
        int nTracePts = GetTraceTotPoints();
        int nVariables = physarray.num_elements();

        const Array<OneD, const int> &traceBndMap
            = m_fields[0]->GetTraceBndMap();

        // Get physical values of the forward trace
        Array<OneD, Array<OneD, NekDouble> > Fwd(nVariables);
        for (i = 0; i < nVariables; ++i)
        {
            Fwd[i] = Array<OneD, NekDouble>(nTracePts);
            m_fields[i]->ExtractTracePhys(physarray[i], Fwd[i]);
        }

        // Adjust the physical values of the trace to
        // take user defined boundaries into account
        int e, id1, id2, nBCEdgePts, eMax;

        eMax = m_fields[0]->GetBndCondExpansions()[bcRegion]->GetExpSize();

        for (e = 0; e < eMax; ++e)
        {
            nBCEdgePts = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetExp(e)->GetTotPoints();
            id1  = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetPhys_Offset(e);
            id2 = m_fields[0]->GetTrace()->GetPhys_Offset(traceBndMap[cnt+e]);

            for (i = 0; i < m_spacedim; i++)
            {
                Vmath::Neg(nBCEdgePts, &Fwd[i+1][id2], 1);
            }

            // Copy boundary adjusted values into the boundary expansion
            for (i = 0; i < nVariables; ++i)
            {
                Vmath::Vcopy(nBCEdgePts, &Fwd[i][id2], 1,
                             &(m_fields[i]->GetBndCondExpansions()[bcRegion]->
                               UpdatePhys())[id1], 1);
            }
        }
    }

    /**
     * @brief Simmetry boundary conditions for compressible flow problems.
     */
    void CompressibleFlowSystem::SymmetryBC(
        int                                      bcRegion,
        int                                      cnt,
        Array<OneD, Array<OneD, NekDouble> >    &physarray)
    {
        int i;
        int nTracePts = GetTraceTotPoints();
        int nVariables = physarray.num_elements();

        const Array<OneD, const int> &traceBndMap
            = m_fields[0]->GetTraceBndMap();

        // Get physical values of the forward trace (from exp to phys)
        Array<OneD, Array<OneD, NekDouble> > Fwd(nVariables);
        for (i = 0; i < nVariables; ++i)
        {
            Fwd[i] = Array<OneD, NekDouble>(nTracePts);
            m_fields[i]->ExtractTracePhys(physarray[i], Fwd[i]);
        }

        int e, id1, id2, nBCEdgePts, eMax;

        eMax = m_fields[0]->GetBndCondExpansions()[bcRegion]->GetExpSize();

        for (e = 0; e < eMax; ++e)
        {
            nBCEdgePts = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetExp(e)->GetTotPoints();
            id1  = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetPhys_Offset(e);
            id2 = m_fields[0]->GetTrace()->GetPhys_Offset(traceBndMap[cnt+e]);

            // For 2D/3D, define: v* = v - 2(v.n)n
            Array<OneD, NekDouble> tmp(nBCEdgePts, 0.0);

            // Calculate (v.n)
            for (i = 0; i < m_spacedim; ++i)
            {
                Vmath::Vvtvp(nBCEdgePts,
                             &Fwd[1+i][id2], 1,
                             &m_traceNormals[i][id2], 1,
                             &tmp[0], 1,
                             &tmp[0], 1);
            }

            // Calculate 2.0(v.n)
            Vmath::Smul(nBCEdgePts, -2.0, &tmp[0], 1, &tmp[0], 1);

            // Calculate v* = v - 2.0(v.n)n
            for (i = 0; i < m_spacedim; ++i)
            {
                Vmath::Vvtvp(nBCEdgePts,
                             &tmp[0], 1,
                             &m_traceNormals[i][id2], 1,
                             &Fwd[1+i][id2], 1,
                             &Fwd[1+i][id2], 1);
            }

            // Copy boundary adjusted values into the boundary expansion
            for (i = 0; i < nVariables; ++i)
            {
                Vmath::Vcopy(nBCEdgePts, &Fwd[i][id2], 1,
                             &(m_fields[i]->GetBndCondExpansions()[bcRegion]->
                               UpdatePhys())[id1], 1);
            }
        }
    }

    /**
     * @brief Outflow characteristic boundary conditions for compressible
     * flow problems.
     */
    void CompressibleFlowSystem::RiemannInvariantBC(
        int                                   bcRegion,
        int                                   cnt,
        Array<OneD, Array<OneD, NekDouble> > &physarray)
    {
        int i, j;
        int nTracePts = GetTraceTotPoints();
        int nVariables = physarray.num_elements();
        int nDimensions = m_spacedim;

        const Array<OneD, const int> &traceBndMap
            = m_fields[0]->GetTraceBndMap();

        NekDouble gamma            = m_gamma;
        NekDouble gammaInv         = 1.0 / gamma;
        NekDouble gammaMinusOne    = gamma - 1.0;
        NekDouble gammaMinusOneInv = 1.0 / gammaMinusOne;

        Array<OneD, NekDouble> tmp1 (nTracePts, 0.0);
        Array<OneD, NekDouble> tmp2 (nTracePts, 0.0);
        Array<OneD, NekDouble> VnInf(nTracePts, 0.0);
        Array<OneD, NekDouble> velInf(nDimensions, 0.0);

        // Computing the normal velocity for characteristics coming
        // from outside the computational domain
        velInf[0] = m_uInf;
        Vmath::Smul(nTracePts, m_uInf, m_traceNormals[0], 1, VnInf, 1);
        if (nDimensions == 2 || nDimensions == 3)
        {
            velInf[1] = m_vInf;
            Vmath::Smul(nTracePts, m_vInf, m_traceNormals[0], 1, tmp1, 1);
            Vmath::Vadd(nTracePts, VnInf, 1, tmp1, 1, VnInf, 1);
        }
        if (nDimensions == 3)
        {
            velInf[2] = m_wInf;
            Vmath::Smul(nTracePts, m_wInf, m_traceNormals[0], 1, tmp2, 1);
            Vmath::Vadd(nTracePts, VnInf, 1, tmp2, 1, VnInf, 1);
        }

        // Get physical values of the forward trace
        Array<OneD, Array<OneD, NekDouble> > Fwd(nVariables);
        for (i = 0; i < nVariables; ++i)
        {
            Fwd[i] = Array<OneD, NekDouble>(nTracePts);
            m_fields[i]->ExtractTracePhys(physarray[i], Fwd[i]);
        }

        // Computing the normal velocity for characteristics coming
        // from inside the computational domain
        Array<OneD, NekDouble > Vn (nTracePts, 0.0);
        Array<OneD, NekDouble > Vel(nTracePts, 0.0);
        for (i = 0; i < nDimensions; ++i)
        {
            Vmath::Vdiv(nTracePts, Fwd[i+1], 1, Fwd[0], 1, Vel, 1);
            Vmath::Vvtvp(nTracePts, m_traceNormals[i], 1, Vel, 1, Vn, 1, Vn, 1);
        }

        // Computing the absolute value of the velocity in order to compute the
        // Mach number to decide whether supersonic or subsonic
        Array<OneD, NekDouble > absVel(nTracePts, 0.0);
        for (i = 0; i < nDimensions; ++i)
        {
            Vmath::Vdiv(nTracePts, Fwd[i+1], 1, Fwd[0], 1, tmp1, 1);
            Vmath::Vmul(nTracePts, tmp1, 1, tmp1, 1, tmp1, 1);
            Vmath::Vadd(nTracePts, tmp1, 1, absVel, 1, absVel, 1);
        }
        Vmath::Vsqrt(nTracePts, absVel, 1, absVel, 1);

        // Get speed of sound
        Array<OneD, NekDouble > soundSpeed(nTracePts);
        Array<OneD, NekDouble > pressure  (nTracePts);

        for (i = 0; i < nTracePts; i++)
        {
            if (m_spacedim == 1)
            {
                pressure[i] = (gammaMinusOne) * (Fwd[2][i] -
                               0.5 * (Fwd[1][i] * Fwd[1][i] / Fwd[0][i]));
            }
            else if (m_spacedim == 2)
            {
                pressure[i] = (gammaMinusOne) * (Fwd[3][i] -
                               0.5 * (Fwd[1][i] * Fwd[1][i] / Fwd[0][i] +
                               Fwd[2][i] * Fwd[2][i] / Fwd[0][i]));
            }
            else
            {
                pressure[i] = (gammaMinusOne) * (Fwd[4][i] -
                               0.5 * (Fwd[1][i] * Fwd[1][i] / Fwd[0][i] +
                               Fwd[2][i] * Fwd[2][i] / Fwd[0][i] +
                               Fwd[3][i] * Fwd[3][i] / Fwd[0][i]));
            }

            soundSpeed[i] = sqrt(gamma * pressure[i] / Fwd[0][i]);
        }

        // Get Mach
        Array<OneD, NekDouble > Mach(nTracePts, 0.0);
        Vmath::Vdiv(nTracePts, Vn, 1, soundSpeed, 1, Mach, 1);
        Vmath::Vabs(nTracePts, Mach, 1, Mach, 1);

        // Auxiliary variables
        int eMax;
        int e, id1, id2, nBCEdgePts, pnt;
        NekDouble cPlus, rPlus, cMinus, rMinus, VDBC, VNBC;
        Array<OneD, NekDouble> velBC(nDimensions, 0.0);
        Array<OneD, NekDouble> rhoVelBC(nDimensions, 0.0);
        NekDouble rhoBC, EBC, cBC, sBC, pBC;

        eMax = m_fields[0]->GetBndCondExpansions()[bcRegion]->GetExpSize();

        // Loop on bcRegions
        for (e = 0; e < eMax; ++e)
        {
            nBCEdgePts = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetExp(e)->GetNumPoints(0);

            id1 = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetPhys_Offset(e);
            id2 = m_fields[0]->GetTrace()->GetPhys_Offset(traceBndMap[cnt+e]);

            // Loop on the points of the bcRegion
            for (i = 0; i < nBCEdgePts; i++)
            {
                pnt = id2+i;

                // Impose inflow Riemann invariant
                if (Vn[pnt] <= 0.0)
                {
                    // Subsonic flows
                    if (Mach[pnt] < 1.00)
                    {
                        // + Characteristic from inside
                        cPlus = sqrt(gamma * pressure[pnt] / Fwd[0][pnt]);
                        rPlus = Vn[pnt] + 2.0 * cPlus * gammaMinusOneInv;

                        // - Characteristic from boundary
                        cMinus = sqrt(gamma * m_pInf / m_rhoInf);
                        rMinus = VnInf[pnt] - 2.0 * cMinus * gammaMinusOneInv;
                    }
                    else
                    {
                        // + Characteristic from inside
                        cPlus = sqrt(gamma * m_pInf / m_rhoInf);
                        rPlus = VnInf[pnt] + 2.0 * cPlus * gammaMinusOneInv;

                        // + Characteristic from inside
                        cMinus = sqrt(gamma * m_pInf / m_rhoInf);
                        rMinus = VnInf[pnt] - 2.0 * cPlus * gammaMinusOneInv;
                    }

                    // Riemann boundary variables
                    VNBC = 0.5 * (rPlus + rMinus);
                    cBC = 0.25 * gammaMinusOne * (rPlus - rMinus);
                    VDBC = VNBC - VnInf[pnt];

                    // Thermodynamic boundary variables
                    sBC = m_pInf / (pow(m_rhoInf, gamma));
                    rhoBC = pow((cBC * cBC) / (gamma * sBC), gammaMinusOneInv);
                    pBC = rhoBC * cBC * cBC * gammaInv;

                    // Kinetic energy initialiasation
                    NekDouble EkBC = 0.0;

                    // Boundary velocities
                    for ( j = 0; j < nDimensions; ++j)
                    {
                        velBC[j] = velInf[j] + VDBC * m_traceNormals[j][pnt];
                        rhoVelBC[j] = rhoBC * velBC[j];
                        EkBC += 0.5 * rhoBC * velBC[j]*velBC[j];
                    }

                    // Boundary energy
                    EBC = pBC * gammaMinusOneInv + EkBC;

                    // Imposing Riemann Invariant boundary conditions
                    (m_fields[0]->GetBndCondExpansions()[bcRegion]->
                     UpdatePhys())[id1+i] = rhoBC;
                    for (j = 0; j < nDimensions; ++j)
                    {
                        (m_fields[j+1]->GetBndCondExpansions()[bcRegion]->
                         UpdatePhys())[id1+i] = rhoVelBC[j];
                    }
                    (m_fields[nDimensions+1]->GetBndCondExpansions()[bcRegion]->
                     UpdatePhys())[id1+i] = EBC;

                }
                else // Impose outflow Riemann invariant
                {
                    // Subsonic flows
                    if (Mach[pnt] < 1.00)
                    {
                        // + Characteristic from inside
                        cPlus = sqrt(gamma * pressure[pnt] / Fwd[0][pnt]);
                        rPlus = Vn[pnt] + 2.0 * cPlus * gammaMinusOneInv;

                        // - Characteristic from boundary
                        cMinus = sqrt(gamma * m_pInf / m_rhoInf);
                        rMinus = VnInf[pnt] - 2.0 * cMinus * gammaMinusOneInv;
                    }
                    else
                    {
                        // + Characteristic from inside
                        cPlus = sqrt(gamma * pressure[pnt] / Fwd[0][pnt]);
                        rPlus = Vn[pnt] + 2.0 * cPlus * gammaMinusOneInv;

                        // + Characteristic from inside
                        cMinus = sqrt(gamma * pressure[pnt] / Fwd[0][pnt]);
                        rMinus = Vn[pnt] - 2.0 * cPlus * gammaMinusOneInv;
                    }

                    // Riemann boundary variables
                    VNBC = 0.5 * (rPlus + rMinus);
                    cBC = 0.25 * gammaMinusOne * (rPlus - rMinus);
                    VDBC = VNBC - Vn[pnt];

                    // Thermodynamic boundary variables
                    sBC = pressure[pnt] / (pow(Fwd[0][pnt], gamma));
                    rhoBC = pow((cBC * cBC) / (gamma * sBC), gammaMinusOneInv);
                    pBC = rhoBC * cBC * cBC * gammaInv;

                    // Kinetic energy initialiasation
                    NekDouble EkBC = 0.0;

                    // Boundary velocities
                    for ( j = 0; j < nDimensions; ++j)
                    {
                        velBC[j] = Fwd[j+1][pnt] / Fwd[0][pnt] +
                                    VDBC * m_traceNormals[j][pnt];
                        rhoVelBC[j] = rhoBC * velBC[j];
                        EkBC += 0.5 * rhoBC * velBC[j]*velBC[j];
                    }

                    // Boundary energy
                    EBC = pBC * gammaMinusOneInv + EkBC;

                    // Imposing Riemann Invariant boundary conditions
                    (m_fields[0]->GetBndCondExpansions()[bcRegion]->
                     UpdatePhys())[id1+i] = rhoBC;
                    for (j = 0; j < nDimensions; ++j)
                    {
                        (m_fields[j+1]->GetBndCondExpansions()[bcRegion]->
                         UpdatePhys())[id1+i] = rhoVelBC[j];
                    }
                    (m_fields[nDimensions+1]->GetBndCondExpansions()[bcRegion]->
                     UpdatePhys())[id1+i] = EBC;
                }
            }
        }
    }

    /**
     * @brief Extrapolation of order 0 for all the variables such that,
     * at the boundaries, a trivial Riemann problem is solved.
     */
    void CompressibleFlowSystem::ExtrapOrder0BC(
        int                                   bcRegion,
        int                                   cnt,
        Array<OneD, Array<OneD, NekDouble> > &physarray)
    {
        int i, j;
        int e, pnt;
        int id1, id2, nBCEdgePts;
        int nTracePts = GetTraceTotPoints();
        int nVariables = physarray.num_elements();
        int nDimensions = m_spacedim;

        const Array<OneD, const int> &traceBndMap
            = m_fields[0]->GetTraceBndMap();

        // Get physical values of the forward trace
        Array<OneD, Array<OneD, NekDouble> > Fwd(nVariables);
        for (i = 0; i < nVariables; ++i)
        {
            Fwd[i] = Array<OneD, NekDouble>(nTracePts);
            m_fields[i]->ExtractTracePhys(physarray[i], Fwd[i]);
        }

        int eMax;

        eMax = m_fields[0]->GetBndCondExpansions()[bcRegion]->GetExpSize();

        // Loop on bcRegions
        for (e = 0; e < eMax; ++e)
        {
            nBCEdgePts = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetExp(e)->GetNumPoints(0);
            id1 = m_fields[0]->GetBndCondExpansions()[bcRegion]->
                GetPhys_Offset(e) ;
            id2 = m_fields[0]->GetTrace()->GetPhys_Offset(traceBndMap[cnt+e]);

            // Loop on points of bcRegion 'e'
            for (i = 0; i < nBCEdgePts; i++)
            {
                pnt = id2+i;

                // Setting up bcs for density
                (m_fields[0]->GetBndCondExpansions()[bcRegion]->
                    UpdatePhys())[id1+i] = Fwd[0][pnt];

                // Setting up bcs for velocities
                for (j = 1; j <=nDimensions; ++j)
                {
                    (m_fields[j]->GetBndCondExpansions()[bcRegion]->
                     UpdatePhys())[id1+i] = Fwd[j][pnt];
                }

                // Setting up bcs for energy
                (m_fields[nVariables-1]->GetBndCondExpansions()[bcRegion]->
                    UpdatePhys())[id1+i] = Fwd[nVariables-1][pnt];
            }
        }
    }

    /**
     * @brief Return the flux vector for the compressible Euler equations.
     *
     * @param i           Component of the flux vector to calculate.
     * @param physfield   Fields.
     * @param flux        Resulting flux.
     */
    void CompressibleFlowSystem::GetFluxVector(
        const Array<OneD, Array<OneD, NekDouble> >               &physfield,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &flux)
    {
        int i, j;
        int nq = physfield[0].num_elements();

        Array<OneD, NekDouble> pressure(nq);
        Array<OneD, Array<OneD, NekDouble> > velocity(m_spacedim);

        // Flux vector for the rho equation
        for (i = 0; i < m_spacedim; ++i)
        {
            velocity[i] = Array<OneD, NekDouble>(nq);
            Vmath::Vcopy(nq, physfield[i+1], 1, flux[0][i], 1);
        }

        GetVelocityVector(physfield, velocity);
        GetPressure      (physfield, velocity, pressure);

        // Flux vector for the velocity fields
        for (i = 0; i < m_spacedim; ++i)
        {
            for (j = 0; j < m_spacedim; ++j)
            {
                Vmath::Vmul(nq, velocity[j], 1, physfield[i+1], 1,
                            flux[i+1][j], 1);
            }

            // Add pressure to appropriate field
            Vmath::Vadd(nq, flux[i+1][i], 1, pressure, 1, flux[i+1][i], 1);
        }

        // Flux vector for energy.
        Vmath::Vadd(nq, physfield[m_spacedim+1], 1, pressure, 1,
                    pressure, 1);

        for (j = 0; j < m_spacedim; ++j)
        {
            Vmath::Vmul(nq, velocity[j], 1, pressure, 1,
                        flux[m_spacedim+1][j], 1);
        }
    }


    /**
     * @brief Return the flux vector for the compressible Euler equations
     * by using the de-aliasing technique.
     *
     * @param i           Component of the flux vector to calculate.
     * @param physfield   Fields.
     * @param flux        Resulting flux.
     */
    void CompressibleFlowSystem::GetFluxVectorDeAlias(
        const Array<OneD, Array<OneD, NekDouble> >               &physfield,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &flux)
    {
        int i, j;
        int nq = physfield[0].num_elements();
        int nVariables = m_fields.num_elements();

        // Factor to rescale 1d points in dealiasing
        NekDouble OneDptscale = 2;
        nq = m_fields[0]->Get1DScaledTotPoints(OneDptscale);

        Array<OneD, NekDouble> pressure(nq);
        Array<OneD, Array<OneD, NekDouble> > velocity(m_spacedim);

        Array<OneD, Array<OneD, NekDouble> > physfield_interp(nVariables);
        Array<OneD, Array<OneD, Array<OneD, NekDouble> > > flux_interp(
                                                            nVariables);

        for (i = 0; i < nVariables; ++ i)
        {
            physfield_interp[i] = Array<OneD, NekDouble>(nq);
            flux_interp[i] = Array<OneD, Array<OneD, NekDouble> >(m_spacedim);
            m_fields[0]->PhysInterp1DScaled(
                OneDptscale, physfield[i], physfield_interp[i]);

            for (j = 0; j < m_spacedim; ++j)
            {
                flux_interp[i][j] = Array<OneD, NekDouble>(nq);
            }
        }

        // Flux vector for the rho equation
        for (i = 0; i < m_spacedim; ++i)
        {
            velocity[i] = Array<OneD, NekDouble>(nq);

            // Galerkin project solution back to original space
            m_fields[0]->PhysGalerkinProjection1DScaled(
                OneDptscale, physfield_interp[i+1], flux[0][i]);
        }

        GetVelocityVector(physfield_interp, velocity);
        GetPressure      (physfield_interp, velocity, pressure);

        // Evaluation of flux vector for the velocity fields
        for (i = 0; i < m_spacedim; ++i)
        {
            for (j = 0; j < m_spacedim; ++j)
            {
                Vmath::Vmul(nq, velocity[j], 1, physfield_interp[i+1], 1,
                            flux_interp[i+1][j], 1);
            }

            // Add pressure to appropriate field
            Vmath::Vadd(nq, flux_interp[i+1][i], 1, pressure,1,
                        flux_interp[i+1][i], 1);
        }

        // Galerkin project solution back to origianl space
        for (i = 0; i < m_spacedim; ++i)
        {
            for (j = 0; j < m_spacedim; ++j)
            {
                m_fields[0]->PhysGalerkinProjection1DScaled(
                    OneDptscale, flux_interp[i+1][j], flux[i+1][j]);
            }
        }

        // Evaluation of flux vector for energy
        Vmath::Vadd(nq, physfield_interp[m_spacedim+1], 1, pressure, 1,
                    pressure, 1);

        for (j = 0; j < m_spacedim; ++j)
        {
            Vmath::Vmul(nq, velocity[j], 1, pressure, 1,
                        flux_interp[m_spacedim+1][j], 1);

            // Galerkin project solution back to origianl space
            m_fields[0]->PhysGalerkinProjection1DScaled(
                OneDptscale,
                flux_interp[m_spacedim+1][j],
                flux[m_spacedim+1][j]);
        }
    }


    /**
     * @brief Return the flux vector for the LDG diffusion problem.
     * \todo Complete the viscous flux vector
     */
    void CompressibleFlowSystem::GetViscousFluxVector(
        const Array<OneD, Array<OneD, NekDouble> >               &physfield,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &derivativesO1,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &viscousTensor)
    {
        int j, k;
        int nVariables = m_fields.num_elements();
        int nPts       = physfield[0].num_elements();

        // Stokes hypotesis
        const NekDouble lambda = -2.0/3.0;

        // Auxiliary variables
        Array<OneD, NekDouble > mu    (nPts, 0.0);
        Array<OneD, NekDouble > mu2   (nPts, 0.0);
        Array<OneD, NekDouble > divVel(nPts, 0.0);

        // Variable viscosity through the Sutherland's law
        if (m_ViscosityType == "Variable")
        {
            GetDynamicViscosity(physfield[nVariables-2], mu);
        }
        else
        {
            Vmath::Fill(nPts, m_mu, &mu[0], 1);
        }

        // Computing diagonal terms of viscous stress tensor
        Array<OneD, Array<OneD, NekDouble> > tmp(m_spacedim);
        Array<OneD, Array<OneD, NekDouble> > Sgg(m_spacedim);

        // mu2 = 2 * mu
        Vmath::Smul(nPts, 2.0, &mu[0], 1, &mu2[0], 1);

        // Velocity divergence
        for (j = 0; j < m_spacedim; ++j)
        {
            Vmath::Vadd(nPts, &divVel[0], 1, &derivativesO1[j][j][0], 1,
                        &divVel[0], 1);
        }

        // Velocity divergence scaled by lambda * mu
        Vmath::Smul(nPts, lambda, &divVel[0], 1, &divVel[0], 1);
        Vmath::Vmul(nPts, &mu[0], 1, &divVel[0], 1, &divVel[0], 1);

        // Diagonal terms of viscous stress tensor (Sxx, Syy, Szz)
        // Sjj = 2 * mu * du_j/dx_j - (2 / 3) * mu * sum_j(du_j/dx_j)
        for (j = 0; j < m_spacedim; ++j)
        {
            tmp[j] = Array<OneD, NekDouble>(nPts, 0.0);
            Sgg[j] = Array<OneD, NekDouble>(nPts, 0.0);

            Vmath::Vmul(nPts, &mu2[0], 1, &derivativesO1[j][j][0], 1,
                        &tmp[j][0], 1);

            Vmath::Vadd(nPts, &tmp[j][0], 1, &divVel[0], 1, &Sgg[j][0], 1);
        }

        // Extra diagonal terms of viscous stress tensor (Sxy, Sxz, Syz)
        // Note: they exist for 2D and 3D problems only
        Array<OneD, NekDouble > Sxy(nPts, 0.0);
        Array<OneD, NekDouble > Sxz(nPts, 0.0);
        Array<OneD, NekDouble > Syz(nPts, 0.0);

        if (m_spacedim == 2)
        {
            // Sxy = (du/dy + dv/dx)
            Vmath::Vadd(nPts, &derivativesO1[0][1][0], 1,
                        &derivativesO1[1][0][0], 1, &Sxy[0], 1);

            // Sxy = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxy[0], 1, &Sxy[0], 1);
        }
        else if (m_spacedim == 3)
        {
            // Sxy = (du/dy + dv/dx)
            Vmath::Vadd(nPts, &derivativesO1[0][1][0], 1,
                        &derivativesO1[1][0][0], 1, &Sxy[0], 1);

            // Sxz = (du/dz + dw/dx)
            Vmath::Vadd(nPts, &derivativesO1[0][2][0], 1,
                        &derivativesO1[2][0][0], 1, &Sxz[0], 1);

            // Syz = (dv/dz + dw/dy)
            Vmath::Vadd(nPts, &derivativesO1[1][2][0], 1,
                        &derivativesO1[2][1][0], 1, &Syz[0], 1);

            // Sxy = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxy[0], 1, &Sxy[0], 1);

            // Sxz = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxz[0], 1, &Sxz[0], 1);

            // Syz = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Syz[0], 1, &Syz[0], 1);
        }

        // Energy-related terms
        Array<OneD, NekDouble > STx(nPts, 0.0);
        Array<OneD, NekDouble > STy(nPts, 0.0);
        Array<OneD, NekDouble > STz(nPts, 0.0);

        // Building the viscous flux vector

        // Viscous flux vector for the rho equation
        for (k = 0; k < m_spacedim; ++k)
        {
            Vmath::Zero(nPts, viscousTensor[k][0], 1);
        }

        if (m_spacedim == 1)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);

            // u * Sxx
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sgg[0][0], 1, &STx[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[0][1][0], 1,
                        &tmp1[0], 1);

            // STx = u * Sxx + (K / mu) * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
        }
        else if (m_spacedim == 2)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);
            Array<OneD, NekDouble > tmp2(nPts, 0.0);

            // Computation of STx

            // u * Sxx
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sgg[0][0], 1, &STx[0], 1);

            // v * Sxy
            Vmath::Vmul(nPts, &physfield[1][0], 1, &Sxy[0], 1, &tmp1[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[0][2][0], 1,
                        &tmp2[0], 1);

            // STx = u * Sxx + v * Sxy + K * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp2[0], 1, &STx[0], 1);

            // Computation of STy

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);

            // v * Syy
            Vmath::Vmul(nPts, &physfield[1][0], 1, &Sgg[1][0], 1, &STy[0], 1);

            // u * Sxy
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sxy[0], 1, &tmp1[0], 1);

            // k * dT/dy
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[1][2][0], 1,
                        &tmp2[0], 1);

            // STy = v * Syy + u * Sxy + K * dT/dy
            Vmath::Vadd(nPts, &STy[0], 1, &tmp1[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp2[0], 1, &STy[0], 1);
        }
        else if (m_spacedim == 3)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);
            Array<OneD, NekDouble > tmp2(nPts, 0.0);
            Array<OneD, NekDouble > tmp3(nPts, 0.0);

            // Computation of STx

            // u * Sxx
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sgg[0][0], 1, &STx[0], 1);

            // v * Sxy
            Vmath::Vmul(nPts, &physfield[1][0], 1, &Sxy[0], 1, &tmp1[0], 1);

            // v * Sxz
            Vmath::Vmul(nPts, &physfield[2][0], 1, &Sxz[0], 1, &tmp2[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[0][3][0], 1,
                        &tmp3[0], 1);

            // STx = u * Sxx + v * Sxy + w * Sxz + (K / mu) * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp2[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp3[0], 1, &STx[0], 1);

            // Computation of STy

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);
            Vmath::Zero(nPts, &tmp3[0], 1);

            // v * Syy
            Vmath::Vmul(nPts, &physfield[1][0], 1, &Sgg[1][0], 1, &STy[0], 1);

            // u * Sxy
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sxy[0], 1, &tmp1[0], 1);

            // w * Syz
            Vmath::Vmul(nPts, &physfield[2][0], 1, &Syz[0], 1, &tmp2[0], 1);

            // k * dT/dy
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[1][3][0], 1,
                        &tmp3[0], 1);

            // STy = v * Syy + u * Sxy + w * Syz + K * dT/dy
            Vmath::Vadd(nPts, &STy[0], 1, &tmp1[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp2[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp3[0], 1, &STy[0], 1);

            // Computation of STz

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);
            Vmath::Zero(nPts, &tmp3[0], 1);

            // w * Szz
            Vmath::Vmul(nPts, &physfield[2][0], 1, &Sgg[2][0], 1, &STz[0], 1);

            // u * Sxz
            Vmath::Vmul(nPts, &physfield[0][0], 1, &Sxz[0], 1, &tmp1[0], 1);

            // v * Syz
            Vmath::Vmul(nPts, &physfield[1][0], 1, &Syz[0], 1, &tmp2[0], 1);

            // k * dT/dz
            Vmath::Smul(nPts, m_thermalConductivity, &derivativesO1[2][3][0], 1,
                        &tmp3[0], 1);

            // STz = w * Szz + u * Sxz + v * Syz + K * dT/dz
            Vmath::Vadd(nPts, &STz[0], 1, &tmp1[0], 1, &STz[0], 1);
            Vmath::Vadd(nPts, &STz[0], 1, &tmp2[0], 1, &STz[0], 1);
            Vmath::Vadd(nPts, &STz[0], 1, &tmp3[0], 1, &STz[0], 1);
        }

        switch (m_spacedim)
        {
            case 1:
            {
                // f_11v = f_rho = 0
                Vmath::Zero(nPts, &viscousTensor[0][0][0], 1);

                // f_21v = f_rhou
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor[0][1][0], 1);

                // f_31v = f_E
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor[0][2][0], 1);
                break;
            }
            case 2:
            {
                // f_11v = f_rho1 = 0
                Vmath::Zero(nPts, &viscousTensor[0][0][0], 1);
                // f_12v = f_rho2 = 0
                Vmath::Zero(nPts, &viscousTensor[1][0][0], 1);

                // f_21v = f_rhou1
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor[0][1][0], 1);
                // f_22v = f_rhou2
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor[1][1][0], 1);

                // f_31v = f_rhov1
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor[0][2][0], 1);
                // f_32v = f_rhov2
                Vmath::Vcopy(nPts, &Sgg[1][0], 1, &viscousTensor[1][2][0], 1);

                // f_41v = f_E1
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor[0][3][0], 1);
                // f_42v = f_E2
                Vmath::Vcopy(nPts, &STy[0], 1, &viscousTensor[1][3][0], 1);
                break;
            }
            case 3:
            {
                // f_11v = f_rho1 = 0
                Vmath::Zero(nPts, &viscousTensor[0][0][0], 1);
                // f_12v = f_rho2 = 0
                Vmath::Zero(nPts, &viscousTensor[1][0][0], 1);
                // f_13v = f_rho3 = 0
                Vmath::Zero(nPts, &viscousTensor[2][0][0], 1);

                // f_21v = f_rhou1
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor[0][1][0], 1);
                // f_22v = f_rhou2
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor[1][1][0], 1);
                // f_23v = f_rhou3
                Vmath::Vcopy(nPts, &Sxz[0],    1, &viscousTensor[2][1][0], 1);

                // f_31v = f_rhov1
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor[0][2][0], 1);
                // f_32v = f_rhov2
                Vmath::Vcopy(nPts, &Sgg[1][0], 1, &viscousTensor[1][2][0], 1);
                // f_33v = f_rhov3
                Vmath::Vcopy(nPts, &Syz[0],    1, &viscousTensor[2][2][0], 1);

                // f_31v = f_rhow1
                Vmath::Vcopy(nPts, &Sxz[0],    1, &viscousTensor[0][3][0], 1);
                // f_32v = f_rhow2
                Vmath::Vcopy(nPts, &Syz[0],    1, &viscousTensor[1][3][0], 1);
                // f_33v = f_rhow3
                Vmath::Vcopy(nPts, &Sgg[2][0], 1, &viscousTensor[2][3][0], 1);

                // f_41v = f_E1
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor[0][4][0], 1);
                // f_42v = f_E2
                Vmath::Vcopy(nPts, &STy[0], 1, &viscousTensor[1][4][0], 1);
                // f_43v = f_E3
                Vmath::Vcopy(nPts, &STz[0], 1, &viscousTensor[2][4][0], 1);
                break;
            }
            default:
            {
                ASSERTL0(false, "Illegal expansion dimension");
            }
        }
    }


    /**
     * @brief Return the flux vector for the LDG diffusion problem.
     * \todo Complete the viscous flux vector
     */
    void CompressibleFlowSystem::GetViscousFluxVectorDeAlias(
        const Array<OneD, Array<OneD, NekDouble> >               &physfield,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &derivativesO1,
              Array<OneD, Array<OneD, Array<OneD, NekDouble> > > &viscousTensor)
    {
#if 0
        int i, j, k;
        int nVariables = m_fields.num_elements();
        int nPts       = physfield[0].num_elements();

        int variables_phys = physfield.num_elements();

        // Factor to rescale 1d points in dealiasing.
        NekDouble OneDptscale = 2;

        // Get number of points to dealias a cubic non-linearity
        nPts = m_fields[0]->Get1DScaledTotPoints(OneDptscale);

        int nVariables_aux = derivativesO1[0].num_elements();

        Array<OneD, Array<OneD, NekDouble> > physfield_interp(variables_phys);
        Array<OneD, Array<OneD, Array<OneD, NekDouble> > > derivativesO1_interp(
                                                                    m_spacedim);
        Array<OneD, Array<OneD, Array<OneD, NekDouble> > >viscousTensor_interp(
                                                                    m_spacedim);

        for (i = 0; i < m_spacedim; ++ i)
        {
            viscousTensor_interp[i] = Array<OneD, Array<OneD, NekDouble> >(
                                                                    nVariables);
            for (j = 0; j < nVariables; ++j)
            {
                viscousTensor_interp[i][j] = Array<OneD, NekDouble>(nPts);
            }
        }

        // Stokes hypotesis
        NekDouble lambda = -2.0/3.0;

        // Auxiliary variables
        Array<OneD, NekDouble > mu         (nPts, 0.0);
        Array<OneD, NekDouble > mu2        (nPts, 0.0);
        Array<OneD, NekDouble > divVel     (nPts, 0.0);
        Array<OneD, NekDouble > pressure   (nPts, 0.0);
        Array<OneD, NekDouble > temperature(nPts, 0.0);

        for (i = 0; i < nVariables; ++i)
        {
            m_fields[0]->PhysInterp1DScaled(
                OneDptscale, physfield[i], fields_interp[i]);
        }

        for (i = 0; i < variables_phys; ++i)
        {
            physfield_interp[i] = Array<OneD, NekDouble> (nPts);

            // Interpolation to higher space
            m_fields[0]->PhysInterp1DScaled(OneDptscale, physfield[i],
                                            physfield_interp[i]);
        }

        for (i = 0; i < m_spacedim; ++i)
        {
            derivativesO1_interp[i] = Array<OneD, Array<OneD, NekDouble> >(
                                                            nVariables_aux);
            for (j = 0; j < nVariables_aux; ++j)
            {
                derivativesO1_interp[i][j] = Array<OneD, NekDouble>(nPts);
                m_fields[0]->PhysInterp1DScaled(
                    OneDptscale, derivativesO1[i][j],
                    derivativesO1_interp[i][j]);
            }
        }

        // Thermodynamic related quantities
        GetPressure(fields_interp, pressure);
        GetTemperature(fields_interp, pressure, temperature);

        // Variable viscosity through the Sutherland's law
        if (m_ViscosityType == "Variable")
        {
            GetDynamicViscosity(fields_interp, mu);
        }
        else
        {
            Vmath::Sadd(nPts, m_mu, &mu[0], 1, &mu[0], 1);
        }

        // Computing diagonal terms of viscous stress tensor
        Array<OneD, Array<OneD, NekDouble> > tmp(m_spacedim);
        Array<OneD, Array<OneD, NekDouble> > Sgg(m_spacedim);

        // mu2 = 2 * mu
        Vmath::Smul(nPts, 2.0, &mu[0], 1, &mu2[0], 1);

        // Velocity divergence
        for (j = 0; j < m_spacedim; ++j)
        {
            Vmath::Vadd(nPts, &divVel[0], 1, &derivativesO1_interp[j][j][0], 1,
                        &divVel[0], 1);
        }

        // Velocity divergence scaled by lambda * mu
        Vmath::Smul(nPts, lambda, &divVel[0], 1, &divVel[0], 1);
        Vmath::Vmul(nPts, &mu[0], 1, &divVel[0], 1, &divVel[0], 1);

        // Digonal terms of viscous stress tensor (Sxx, Syy, Szz)
        // Sjj = 2 * mu * du_j/dx_j - (2 / 3) * mu * sum_j(du_j/dx_j)
        for (j = 0; j < m_spacedim; ++j)
        {
            tmp[j] = Array<OneD, NekDouble>(nPts, 0.0);
            Sgg[j] = Array<OneD, NekDouble>(nPts, 0.0);

            Vmath::Vmul(nPts, &mu2[0], 1, &derivativesO1_interp[j][j][0], 1,
                        &tmp[j][0], 1);

            Vmath::Vadd(nPts, &tmp[j][0], 1, &divVel[0], 1, &Sgg[j][0], 1);
        }

        // Extra diagonal terms of viscous stress tensor (Sxy, Sxz, Syz)
        // Note: they exist for 2D and 3D problems only
        Array<OneD, NekDouble > Sxy(nPts, 0.0);
        Array<OneD, NekDouble > Sxz(nPts, 0.0);
        Array<OneD, NekDouble > Syz(nPts, 0.0);

        if (m_spacedim == 2)
        {
            // Sxy = (du/dy + dv/dx)
            Vmath::Vadd(nPts, &derivativesO1_interp[0][1][0], 1,
                        &derivativesO1_interp[1][0][0], 1, &Sxy[0], 1);

            // Sxy = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxy[0], 1, &Sxy[0], 1);
        }
        else if (m_spacedim == 3)
        {
            // Sxy = (du/dy + dv/dx)
            Vmath::Vadd(nPts, &derivativesO1_interp[0][1][0], 1,
                        &derivativesO1_interp[1][0][0], 1, &Sxy[0], 1);

            // Sxz = (du/dz + dw/dx)
            Vmath::Vadd(nPts, &derivativesO1_interp[0][2][0], 1,
                        &derivativesO1_interp[2][0][0], 1, &Sxz[0], 1);

            // Syz = (dv/dz + dw/dy)
            Vmath::Vadd(nPts, &derivativesO1_interp[1][2][0], 1,
                        &derivativesO1_interp[2][1][0], 1, &Syz[0], 1);

            // Sxy = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxy[0], 1, &Sxy[0], 1);

            // Sxz = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Sxz[0], 1, &Sxz[0], 1);

            // Syz = mu * (du/dy + dv/dx)
            Vmath::Vmul(nPts, &mu[0], 1, &Syz[0], 1, &Syz[0], 1);
        }

        // Energy-related terms
        Array<OneD, NekDouble > STx(nPts, 0.0);
        Array<OneD, NekDouble > STy(nPts, 0.0);
        Array<OneD, NekDouble > STz(nPts, 0.0);
        // Building the viscous flux vector
        if (i == 0)
        {
            // Viscous flux vector for the rho equation
            for (k = 0; k < m_spacedim; ++k)
            {
                Vmath::Zero(nPts, viscousTensor_interp[k][i], 1);
            }
        }

        if (m_spacedim == 1)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);

            // u * Sxx
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sgg[0][0], 1, &STx[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[0][1][0], 1, &tmp1[0], 1);

            // STx = u * Sxx + (K / mu) * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
        }
        else if (m_spacedim == 2)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);
            Array<OneD, NekDouble > tmp2(nPts, 0.0);

            // Computation of STx

            // u * Sxx
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sgg[0][0], 1, &STx[0], 1);

            // v * Sxy
            Vmath::Vmul(nPts, &physfield_interp[1][0], 1,
                        &Sxy[0], 1, &tmp1[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[0][2][0], 1, &tmp2[0], 1);

            // STx = u * Sxx + v * Sxy + K * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp2[0], 1, &STx[0], 1);

            // Computation of STy

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);

            // v * Syy
            Vmath::Vmul(nPts, &physfield_interp[1][0], 1,
                        &Sgg[1][0], 1, &STy[0], 1);

            // u * Sxy
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sxy[0], 1, &tmp1[0], 1);

            // k * dT/dy
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[1][2][0], 1, &tmp2[0], 1);

            // STy = v * Syy + u * Sxy + K * dT/dy
            Vmath::Vadd(nPts, &STy[0], 1, &tmp1[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp2[0], 1, &STy[0], 1);
        }
        else if (m_spacedim == 3)
        {
            Array<OneD, NekDouble > tmp1(nPts, 0.0);
            Array<OneD, NekDouble > tmp2(nPts, 0.0);
            Array<OneD, NekDouble > tmp3(nPts, 0.0);

            // Computation of STx

            // u * Sxx
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sgg[0][0], 1, &STx[0], 1);

            // v * Sxy
            Vmath::Vmul(nPts, &physfield_interp[1][0], 1,
                        &Sxy[0], 1, &tmp1[0], 1);

            // v * Sxy
            Vmath::Vmul(nPts, &physfield_interp[2][0], 1,
                        &Sxz[0], 1, &tmp2[0], 1);

            // k * dT/dx
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[0][3][0], 1, &tmp3[0], 1);

            // STx = u * Sxx + v * Sxy + w * Sxz + (K / mu) * dT/dx
            Vmath::Vadd(nPts, &STx[0], 1, &tmp1[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp2[0], 1, &STx[0], 1);
            Vmath::Vadd(nPts, &STx[0], 1, &tmp3[0], 1, &STx[0], 1);

            // Computation of STy

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);
            Vmath::Zero(nPts, &tmp3[0], 1);

            // v * Syy
            Vmath::Vmul(nPts, &physfield_interp[1][0], 1,
                        &Sgg[1][0], 1, &STy[0], 1);

            // u * Sxy
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sxy[0], 1, &tmp1[0], 1);

            // w * Syz
            Vmath::Vmul(nPts, &physfield_interp[2][0], 1,
                        &Syz[0], 1, &tmp2[0], 1);

            // k * dT/dy
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[1][3][0], 1, &tmp3[0], 1);

            // STy = v * Syy + u * Sxy + w * Syz + K * dT/dy
            Vmath::Vadd(nPts, &STy[0], 1, &tmp1[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp2[0], 1, &STy[0], 1);
            Vmath::Vadd(nPts, &STy[0], 1, &tmp3[0], 1, &STy[0], 1);

            // Computation of STz

            // Re-initialise temporary arrays
            Vmath::Zero(nPts, &tmp1[0], 1);
            Vmath::Zero(nPts, &tmp2[0], 1);
            Vmath::Zero(nPts, &tmp3[0], 1);

            // w * Szz
            Vmath::Vmul(nPts, &physfield_interp[2][0], 1,
                        &Sgg[2][0], 1, &STz[0], 1);

            // u * Sxz
            Vmath::Vmul(nPts, &physfield_interp[0][0], 1,
                        &Sxz[0], 1, &tmp1[0], 1);

            // v * Syz
            Vmath::Vmul(nPts, &physfield_interp[1][0], 1,
                        &Syz[0], 1, &tmp2[0], 1);

            // k * dT/dz
            Vmath::Smul(nPts, m_thermalConductivity,
                        &derivativesO1_interp[2][3][0], 1, &tmp3[0], 1);

            // STz = w * Szz + u * Sxz + v * Syz + K * dT/dz
            Vmath::Vadd(nPts, &STz[0], 1, &tmp1[0], 1, &STz[0], 1);
            Vmath::Vadd(nPts, &STz[0], 1, &tmp2[0], 1, &STz[0], 1);
            Vmath::Vadd(nPts, &STz[0], 1, &tmp3[0], 1, &STz[0], 1);
        }

        switch (m_spacedim)
        {
            case 1:
            {

                int nq = physfield[0].num_elements();
                // f_11v = f_rho = 0
                Vmath::Zero(nq, &viscousTensor_interp[0][0][0], 1);

                // f_21v = f_rhou
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor_interp[0][1][0], 1);

                // f_31v = f_E
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor_interp[0][2][0], 1);
                break;
            }
            case 2:
            {
                int nq = physfield[0].num_elements();
                // f_11v = f_rho1 = 0
                Vmath::Zero(nq, &viscousTensor_interp[0][0][0], 1);
                // f_12v = f_rho2 = 0
                Vmath::Zero(nq, &viscousTensor_interp[1][0][0], 1);

                // f_21v = f_rhou1
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor_interp[0][1][0], 1);
                // f_22v = f_rhou2
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor_interp[1][1][0], 1);

                // f_31v = f_rhov1
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor_interp[0][2][0], 1);
                // f_32v = f_rhov2
                Vmath::Vcopy(nPts, &Sgg[1][0], 1, &viscousTensor_interp[1][2][0], 1);

                // f_41v = f_E1
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor_interp[0][3][0], 1);
                // f_42v = f_E2
                Vmath::Vcopy(nPts, &STy[0], 1, &viscousTensor_interp[1][3][0], 1);
                break;
            }
            case 3:
            {
                int nq = physfield[0].num_elements();
                // f_11v = f_rho1 = 0
                Vmath::Zero(nq, &viscousTensor_interp[0][0][0], 1);
                // f_12v = f_rho2 = 0
                Vmath::Zero(nq, &viscousTensor_interp[1][0][0], 1);
                // f_13v = f_rho3 = 0
                Vmath::Zero(nq, &viscousTensor_interp[2][0][0], 1);

                // f_21v = f_rhou1
                Vmath::Vcopy(nPts, &Sgg[0][0], 1, &viscousTensor_interp[0][1][0], 1);
                // f_22v = f_rhou2
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor_interp[1][1][0], 1);
                // f_23v = f_rhou3
                Vmath::Vcopy(nPts, &Sxz[0],    1, &viscousTensor_interp[2][1][0], 1);

                // f_31v = f_rhov1
                Vmath::Vcopy(nPts, &Sxy[0],    1, &viscousTensor_interp[0][2][0], 1);
                // f_32v = f_rhov2
                Vmath::Vcopy(nPts, &Sgg[1][0], 1, &viscousTensor_interp[1][2][0], 1);
                // f_33v = f_rhov3
                Vmath::Vcopy(nPts, &Syz[0],    1, &viscousTensor_interp[2][2][0], 1);

                // f_31v = f_rhow1
                Vmath::Vcopy(nPts, &Sxz[0],    1, &viscousTensor_interp[0][3][0], 1);
                // f_32v = f_rhow2
                Vmath::Vcopy(nPts, &Syz[0],    1, &viscousTensor_interp[1][3][0], 1);
                // f_33v = f_rhow3
                Vmath::Vcopy(nPts, &Sgg[2][0], 1, &viscousTensor_interp[2][3][0], 1);

                // f_41v = f_E1
                Vmath::Vcopy(nPts, &STx[0], 1, &viscousTensor_interp[0][4][0], 1);
                // f_42v = f_E2
                Vmath::Vcopy(nPts, &STy[0], 1, &viscousTensor_interp[1][4][0], 1);
                // f_43v = f_E3
                Vmath::Vcopy(nPts, &STz[0], 1, &viscousTensor_interp[2][4][0], 1);
                break;
            }
            default:
            {
                ASSERTL0(false, "Illegal expansion dimension");
            }
        }

        for (i = 0; i < m_spacedim; ++i)
        {
            for (j = 1; j < nVariables; ++j)
            {
                // Galerkin project solution back to origianl space
                m_fields[0]->PhysGalerkinProjection1DScaled(
                    OneDptscale,
                    viscousTensor_interp[i][j],
                    viscousTensor[i][j]);
            }
        }
#endif
}

    /**
     * @brief Calculate the pressure field \f$ p =
     * (\gamma-1)(E-\frac{1}{2}\rho\| \mathbf{v} \|^2) \f$ assuming an ideal
     * gas law.
     *
     * @param physfield  Input momentum.
     * @param pressure   Computed pressure field.
     */
    void CompressibleFlowSystem::GetPressure(
        const Array<OneD, const Array<OneD, NekDouble> > &physfield,
              Array<OneD,                   NekDouble>   &pressure)
    {
        int       nBCEdgePts  = physfield[0].num_elements();
        NekDouble alpha = -0.5;

        // Calculate ||rho v||^2
        Vmath::Vmul(nBCEdgePts, physfield[1], 1, physfield[1], 1, pressure, 1);
        for (int i = 1; i < m_spacedim; ++i)
        {
            Vmath::Vvtvp(nBCEdgePts, physfield[1+i], 1, physfield[1+i], 1,
                               pressure,       1, pressure,       1);
        }
        // Divide by rho to get rho*||v||^2
        Vmath::Vdiv (nBCEdgePts, pressure, 1, physfield[0], 1, pressure, 1);
        // pressure <- E - 0.5*pressure
        Vmath::Svtvp(nBCEdgePts, alpha,
                     pressure, 1, physfield[m_spacedim+1], 1, pressure, 1);
        // Multiply by (gamma-1)
        Vmath::Smul (nBCEdgePts, m_gamma-1, pressure, 1, pressure, 1);
    }

    /**
     * @brief Calculate the pressure field \f$ p =
     * (\gamma-1)(E-\frac{1}{2}\rho\| \mathbf{v} \|^2) \f$ assuming an ideal
     * gas law.
     *
     * This is a slightly optimised way to calculate the pressure field which
     * avoids division by the density field if the velocity field has already
     * been calculated.
     *
     * @param physfield  Input momentum.
     * @param velocity   Velocity vector.
     * @param pressure   Computed pressure field.
     */
    void CompressibleFlowSystem::GetPressure(
        const Array<OneD, const Array<OneD, NekDouble> > &physfield,
        const Array<OneD, const Array<OneD, NekDouble> > &velocity,
              Array<OneD,                   NekDouble>   &pressure)
    {
        int nBCEdgePts = physfield[0].num_elements();
        NekDouble alpha = -0.5;

        // Calculate ||\rho v||^2.
        Vmath::Vmul (nBCEdgePts, velocity[0], 1, physfield[1], 1, pressure, 1);
        for (int i = 1; i < m_spacedim; ++i)
        {
            Vmath::Vvtvp(nBCEdgePts, velocity[i], 1, physfield[1+i], 1,
                               pressure,    1, pressure,       1);
        }
        // pressure <- E - 0.5*pressure
        Vmath::Svtvp(nBCEdgePts,     alpha,
                     pressure, 1, physfield[m_spacedim+1], 1, pressure, 1);
        // Multiply by (gamma-1).
        Vmath::Smul (nBCEdgePts, m_gamma-1, pressure, 1, pressure, 1);
    }

    /**
     * @brief Compute the velocity field \f$ \mathbf{v} \f$ given the momentum
     * \f$ \rho\mathbf{v} \f$.
     *
     * @param physfield  Momentum field.
     * @param velocity   Velocity field.
     */
    void CompressibleFlowSystem::GetVelocityVector(
        const Array<OneD, Array<OneD, NekDouble> > &physfield,
              Array<OneD, Array<OneD, NekDouble> > &velocity)
    {
        const int nBCEdgePts = physfield[0].num_elements();

        for (int i = 0; i < m_spacedim; ++i)
        {
            Vmath::Vdiv(nBCEdgePts, physfield[1+i], 1, physfield[0], 1,
                              velocity[i],    1);
        }
    }

    /**
     * @brief Compute the temperature \f$ T = p/\rho R \f$.
     *
     * @param physfield    Input physical field.
     * @param pressure     The pressure field corresponding to physfield.
     * @param temperature  The resulting temperature \f$ T \f$.
     */
    void CompressibleFlowSystem::GetTemperature(
        const Array<OneD, const Array<OneD, NekDouble> > &physfield,
        Array<OneD,                         NekDouble  > &pressure,
        Array<OneD,                         NekDouble  > &temperature)
    {
        const int nq = physfield[0].num_elements();

        Vmath::Vdiv(nq, pressure, 1, physfield[0], 1, temperature, 1);
        Vmath::Smul(nq, 1.0/m_gasConstant, temperature, 1, temperature, 1);
    }

    /**
     * @brief Compute the sound speed \f$ c = sqrt(\gamma p/\rho) \f$.
     *
     * @param physfield    Input physical field.
     * @param pressure     The pressure field corresponding to physfield.
     * @param soundspeed   The resulting sound speed \f$ c \f$.
     */
    void CompressibleFlowSystem::GetSoundSpeed(
        const Array<OneD, Array<OneD, NekDouble> > &physfield,
              Array<OneD,             NekDouble  > &pressure,
              Array<OneD,             NekDouble  > &soundspeed)
    {
        const int nq = m_fields[0]->GetTotPoints();
        Vmath::Vdiv (nq, pressure, 1, physfield[0], 1, soundspeed, 1);
        Vmath::Smul (nq, m_gamma, soundspeed, 1, soundspeed, 1);
        Vmath::Vsqrt(nq, soundspeed, 1, soundspeed, 1);
    }

    /**
     * @brief Compute the mach number \f$ M = \| \mathbf{v} \|^2 / c \f$.
     *
     * @param physfield    Input physical field.
     * @param soundfield   The speed of sound corresponding to physfield.
     * @param mach         The resulting mach number \f$ M \f$.
     */
    void CompressibleFlowSystem::GetMach(
        Array<OneD, Array<OneD, NekDouble> > &physfield,
        Array<OneD,             NekDouble  > &soundspeed,
        Array<OneD,             NekDouble  > &mach)
    {
        const int nBCEdgePts = m_fields[0]->GetTotPoints();

        Vmath::Vmul(nBCEdgePts, physfield[1], 1, physfield[1], 1, mach, 1);

        for (int i = 1; i < m_spacedim; ++i)
        {
            Vmath::Vvtvp(nBCEdgePts, physfield[1+i], 1, physfield[1+i], 1,
                               mach,           1, mach,           1);
        }

        Vmath::Vdiv(nBCEdgePts, mach, 1, physfield[0], 1, mach, 1);
        Vmath::Vdiv(nBCEdgePts, mach, 1, physfield[0], 1, mach, 1);
        Vmath::Vdiv(nBCEdgePts, mach, 1, soundspeed,   1, mach, 1);
    }

    /**
     * @brief Compute the dynamic viscosity using the Sutherland's law
     * \f$ \mu = \mu_star * (T / T_star)^3/2 * (T_star + 110) / (T + 110) \f$,
     * where:   \mu_star = 1.7894 * 10^-5 Kg / (m * s)
     *          T_star   = 288.15 K
     *
     * @param physfield    Input physical field.
     * @param mu           The resulting dynamic viscosity.
     */
    void CompressibleFlowSystem::GetDynamicViscosity(
        const Array<OneD, const NekDouble> &temperature,
              Array<OneD,       NekDouble> &mu)
    {
        const int nPts    = temperature.num_elements();
        NekDouble mu_star = m_mu;
        NekDouble T_star  = m_pInf / (m_rhoInf * m_gasConstant);
        NekDouble ratio;

        for (int i = 0; i < nPts; ++i)
        {
            ratio = temperature[i] / T_star;
            mu[i] = mu_star * pow(ratio, 1.50) *
                    (T_star + 110.0) / (temperature[i] + 110.0);
        }
    }

    /**
     * @brief Calculate the maximum timestep subject to CFL restrictions.
     */
    NekDouble CompressibleFlowSystem::v_GetTimeStep(
        const Array<OneD, const Array<OneD, NekDouble> > &inarray)
    {
        int n;
        int nElements = m_fields[0]->GetExpSize();
        const Array<OneD, int> ExpOrder = GetNumExpModesPerExp();

        Array<OneD, NekDouble> tstep      (nElements, 0.0);
        Array<OneD, NekDouble> stdVelocity(nElements);

        // Get standard velocity to compute the time-step limit
        GetStdVelocity(inarray, stdVelocity);

        // Factors to compute the time-step limit
        NekDouble minLength;
        NekDouble alpha   = MaxTimeStepEstimator();
        NekDouble cLambda = 0.2; // Spencer book-317

        // Loop over elements to compute the time-step limit for each element
        for(n = 0; n < nElements; ++n)
        {
            int npoints = m_fields[0]->GetExp(n)->GetTotPoints();
            Array<OneD, NekDouble> one2D(npoints, 1.0);
            NekDouble Area = m_fields[0]->GetExp(n)->Integral(one2D);

            if (boost::dynamic_pointer_cast<LocalRegions::TriExp>(
                    m_fields[0]->GetExp(n)))
            {
                minLength = 2.0 * sqrt(Area);
            }

            else if (boost::dynamic_pointer_cast<LocalRegions::QuadExp>(
                         m_fields[0]->GetExp(n)))
            {
                minLength = sqrt(Area);
            }
            else if (boost::dynamic_pointer_cast<LocalRegions::HexExp>(
                         m_fields[0]->GetExp(n)))
            {
                minLength = sqrt(Area);
            }

            tstep[n] = m_cflSafetyFactor * alpha * minLength
                     / (stdVelocity[n] * cLambda
                        * (ExpOrder[n] - 1) * (ExpOrder[n] - 1));
        }

        // Get the minimum time-step limit and return the time-step
        NekDouble TimeStep = Vmath::Vmin(nElements, tstep, 1);
        m_comm->AllReduce(TimeStep, LibUtilities::ReduceMin);
        return TimeStep;
    }

    /**
     * @brief Compute the advection velocity in the standard space
     * for each element of the expansion.
     *
     * @param inarray    Momentum field.
     * @param stdV       Standard velocity field.
     */
    void CompressibleFlowSystem::GetStdVelocity(
        const Array<OneD, const Array<OneD, NekDouble> > &inarray,
              Array<OneD,                   NekDouble>   &stdV)
    {
        int nTotQuadPoints = GetTotPoints();
        int n_element      = m_fields[0]->GetExpSize();
        int nBCEdgePts           = 0;

        // Getting the velocity vector on the 2D normal space
        Array<OneD, Array<OneD, NekDouble> > velocity   (m_spacedim);
        Array<OneD, Array<OneD, NekDouble> > stdVelocity(m_spacedim);
        Array<OneD, NekDouble>               pressure   (nTotQuadPoints);
        Array<OneD, NekDouble>               soundspeed (nTotQuadPoints);
        LibUtilities::PointsKeyVector        ptsKeys;
        
        // Zero output array
        Vmath::Zero(stdV.num_elements(), stdV, 1);

        for (int i = 0; i < m_spacedim; ++i)
        {
            velocity   [i] = Array<OneD, NekDouble>(nTotQuadPoints);
            stdVelocity[i] = Array<OneD, NekDouble>(nTotQuadPoints, 0.0);
        }
        GetVelocityVector(inarray, velocity);
        GetPressure      (inarray, velocity, pressure);
        GetSoundSpeed    (inarray, pressure, soundspeed);

        for(int el = 0; el < n_element; ++el)
        { 
            ptsKeys = m_fields[0]->GetExp(el)->GetPointsKeys();

            // Possible bug: not multiply by jacobian??
            const SpatialDomains::GeomFactorsSharedPtr metricInfo =
                m_fields[0]->GetExp(el)->GetGeom()->GetMetricInfo();
            const Array<TwoD, const NekDouble> &gmat = 
                m_fields[0]->GetExp(el)->GetGeom()->GetMetricInfo()
                                                  ->GetDerivFactors(ptsKeys);

            int nq = m_fields[0]->GetExp(el)->GetTotPoints();

            if(metricInfo->GetGtype() == SpatialDomains::eDeformed)
            {
                // d xi/ dx = gmat = 1/J * d x/d xi
                for (int i = 0; i < m_spacedim; ++i)
                {
                    Vmath::Vmul(nq, gmat[i], 1, velocity[0], 1,
                                stdVelocity[i], 1);
                    for (int j = 1; j < m_spacedim; ++j)
                    {
                        Vmath::Vvtvp(nq, gmat[m_spacedim*j+i], 1, velocity[j],
                                     1, stdVelocity[i], 1, stdVelocity[i], 1);
                    }
                }
            }
            else
            {
                for (int i = 0; i < m_spacedim; ++i)
                {
                    Vmath::Smul(nq, gmat[i][0], velocity[0], 1,
                                stdVelocity[i], 1);
                    for (int j = 1; j < m_spacedim; ++j)
                    {
                        Vmath::Svtvp(nq, gmat[m_spacedim*j+i][0], velocity[j],
                                     1, stdVelocity[i], 1, stdVelocity[i], 1);
                    }
                }
            }

            for (int i = 0; i < nq; ++i)
            {
                NekDouble pntVelocity = 0.0;
                for (int j = 0; j < m_spacedim; ++j)
                {
                    pntVelocity += stdVelocity[j][i]*stdVelocity[j][i];
                }
                pntVelocity = sqrt(pntVelocity) + soundspeed[nBCEdgePts];
                if (pntVelocity > stdV[el])
                {
                    stdV[el] = pntVelocity;
                }
                nBCEdgePts++;
            }
        }
    }

    /**
     * @brief Set the denominator to compute the time step when a cfl
     * control is employed. This function is no longer used but is still
     * here for being utilised in the future.
     *
     * @param n   Order of expansion element by element.
     */
    NekDouble CompressibleFlowSystem::GetStabilityLimit(int n)
    {
        ASSERTL0(n <= 20, "Illegal modes dimension for CFL calculation "
                          "(P has to be less then 20)");

        NekDouble CFLDG[21] = {  2.0000,   6.0000,  11.8424,  19.1569,
                                27.8419,  37.8247,  49.0518,  61.4815,
                                75.0797,  89.8181, 105.6700, 122.6200,
                               140.6400, 159.7300, 179.8500, 201.0100,
                               223.1800, 246.3600, 270.5300, 295.6900,
                               321.8300}; //CFLDG 1D [0-20]
        NekDouble CFL = 0.0;

        if (m_projectionType == MultiRegions::eDiscontinuous)
        {
            CFL = CFLDG[n];
        }
        else
        {
            ASSERTL0(false, "Continuous Galerkin stability coefficients "
                            "not introduced yet.");
        }

        return CFL;
    }

    /**
     * @brief Compute the vector of denominators to compute the time step
     * when a cfl control is employed. This function is no longer used but
     * is still here for being utilised in the future.
     *
     * @param ExpOrder   Order of expansion element by element.
     */
    Array<OneD, NekDouble> CompressibleFlowSystem::GetStabilityLimitVector(
        const Array<OneD,int> &ExpOrder)
    {
        int i;
        Array<OneD,NekDouble> returnval(m_fields[0]->GetExpSize(), 0.0);
        for (i =0; i<m_fields[0]->GetExpSize(); i++)
        {
            returnval[i] = GetStabilityLimit(ExpOrder[i]);
        }
        return returnval;
    }
}
