
/* Portions copyright (c) 2006-2013 Stanford University and Simbios.
 * Contributors: Pande Group
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <complex>

#include "SimTKOpenMMCommon.h"
#include "SimTKOpenMMUtilities.h"
#include "CpuNonbondedForce.h"
#include "ReferenceForce.h"
#include "ReferencePME.h"
#include "openmm/internal/SplineFitter.h"
#include "openmm/internal/vectorize.h"

// In case we're using some primitive version of Visual Studio this will
// make sure that erf() and erfc() are defined.
#include "openmm/internal/MSVC_erfc.h"

using namespace std;
using namespace OpenMM;

const float CpuNonbondedForce::TWO_OVER_SQRT_PI = (float) (2/sqrt(PI_M));
const int CpuNonbondedForce::NUM_TABLE_POINTS = 1025;

class CpuNonbondedForce::ComputeDirectTask : public ThreadPool::Task {
public:
    ComputeDirectTask(CpuNonbondedForce& owner) : owner(owner) {
    }
    void execute(ThreadPool& threads, int threadIndex) {
        owner.threadComputeDirect(threads, threadIndex);
    }
    CpuNonbondedForce& owner;
};

/**---------------------------------------------------------------------------------------

   CpuNonbondedForce constructor

   --------------------------------------------------------------------------------------- */

CpuNonbondedForce::CpuNonbondedForce() : cutoff(false), useSwitch(false), periodic(false), ewald(false), pme(false) {
}

  /**---------------------------------------------------------------------------------------

     Set the force to use a cutoff.

     @param distance            the cutoff distance
     @param neighbors           the neighbor list to use
     @param solventDielectric   the dielectric constant of the bulk solvent

     --------------------------------------------------------------------------------------- */

  void CpuNonbondedForce::setUseCutoff(float distance, const CpuNeighborList& neighbors, float solventDielectric) {

    cutoff = true;
    cutoffDistance = distance;
    neighborList = &neighbors;
    krf = pow(cutoffDistance, -3.0f)*(solventDielectric-1.0)/(2.0*solventDielectric+1.0);
    crf = (1.0/cutoffDistance)*(3.0*solventDielectric)/(2.0*solventDielectric+1.0);
  }

/**---------------------------------------------------------------------------------------

   Set the force to use a switching function on the Lennard-Jones interaction.

   @param distance            the switching distance

   --------------------------------------------------------------------------------------- */

void CpuNonbondedForce::setUseSwitchingFunction(float distance) {
    useSwitch = true;
    switchingDistance = distance;
}

  /**---------------------------------------------------------------------------------------

     Set the force to use periodic boundary conditions.  This requires that a cutoff has
     also been set, and the smallest side of the periodic box is at least twice the cutoff
     distance.

     @param boxSize             the X, Y, and Z widths of the periodic box

     --------------------------------------------------------------------------------------- */

  void CpuNonbondedForce::setPeriodic(float* periodicBoxSize) {

    assert(cutoff);
    assert(periodicBoxSize[0] >= 2*cutoffDistance);
    assert(periodicBoxSize[1] >= 2*cutoffDistance);
    assert(periodicBoxSize[2] >= 2*cutoffDistance);
    periodic = true;
    this->periodicBoxSize[0] = periodicBoxSize[0];
    this->periodicBoxSize[1] = periodicBoxSize[1];
    this->periodicBoxSize[2] = periodicBoxSize[2];
  }

  /**---------------------------------------------------------------------------------------

     Set the force to use Ewald summation.

     @param alpha  the Ewald separation parameter
     @param kmaxx  the largest wave vector in the x direction
     @param kmaxy  the largest wave vector in the y direction
     @param kmaxz  the largest wave vector in the z direction

     --------------------------------------------------------------------------------------- */

  void CpuNonbondedForce::setUseEwald(float alpha, int kmaxx, int kmaxy, int kmaxz) {
      alphaEwald = alpha;
      numRx = kmaxx;
      numRy = kmaxy;
      numRz = kmaxz;
      ewald = true;
      tabulateEwaldScaleFactor();
  }

  /**---------------------------------------------------------------------------------------

     Set the force to use Particle-Mesh Ewald (PME) summation.

     @param alpha  the Ewald separation parameter
     @param gridSize the dimensions of the mesh

     --------------------------------------------------------------------------------------- */

  void CpuNonbondedForce::setUsePME(float alpha, int meshSize[3]) {
      alphaEwald = alpha;
      meshDim[0] = meshSize[0];
      meshDim[1] = meshSize[1];
      meshDim[2] = meshSize[2];
      pme = true;
      tabulateEwaldScaleFactor();
  }

  
void CpuNonbondedForce::tabulateEwaldScaleFactor() {
    ewaldDX = cutoffDistance/(NUM_TABLE_POINTS-2);
    ewaldDXInv = 1.0f/ewaldDX;
    vector<double> x(NUM_TABLE_POINTS+1);
    vector<double> y(NUM_TABLE_POINTS+1);
    vector<double> deriv;
    for (int i = 0; i < NUM_TABLE_POINTS+1; i++) {
        double r = i*cutoffDistance/(NUM_TABLE_POINTS-2);
        double alphaR = alphaEwald*r;
        x[i] = r;
        y[i] = erfc(alphaR) + TWO_OVER_SQRT_PI*alphaR*exp(-alphaR*alphaR);
    }
    SplineFitter::createNaturalSpline(x, y, deriv);
    ewaldScaleTable.resize(4*NUM_TABLE_POINTS);
    for (int i = 0; i < NUM_TABLE_POINTS; i++) {
        ewaldScaleTable[4*i] = (float) y[i];
        ewaldScaleTable[4*i+1] = (float) y[i+1];
        ewaldScaleTable[4*i+2] = (float) (deriv[i]*ewaldDX*ewaldDX/6);
        ewaldScaleTable[4*i+3] = (float) (deriv[i+1]*ewaldDX*ewaldDX/6);
    }
}
  
void CpuNonbondedForce::calculateReciprocalIxn(int numberOfAtoms, float* posq, const vector<RealVec>& atomCoordinates,
                                             const vector<pair<float, float> >& atomParameters, const vector<set<int> >& exclusions,
                                             vector<RealVec>& forces, float* totalEnergy) const {
    typedef std::complex<float> d_complex;

    static const float epsilon     =  1.0;

    int kmax                            = (ewald ? std::max(numRx, std::max(numRy,numRz)) : 0);
    float factorEwald              = -1 / (4*alphaEwald*alphaEwald);
    float TWO_PI                   = 2.0 * PI_M;
    float recipCoeff               = (float)(ONE_4PI_EPS0*4*PI_M/(periodicBoxSize[0] * periodicBoxSize[1] * periodicBoxSize[2]) /epsilon);

    if (pme) {
        pme_t pmedata;
        RealOpenMM virial[3][3];
        pme_init(&pmedata, alphaEwald, numberOfAtoms, meshDim, 5, 1);
        vector<RealOpenMM> charges(numberOfAtoms);
        for (int i = 0; i < numberOfAtoms; i++)
            charges[i] = posq[4*i+3];
        RealOpenMM boxSize[3] = {periodicBoxSize[0], periodicBoxSize[1], periodicBoxSize[2]};
        RealOpenMM recipEnergy = 0.0;
        pme_exec(pmedata, atomCoordinates, forces, charges, boxSize, &recipEnergy, virial);
        if (totalEnergy)
            *totalEnergy += recipEnergy;
        pme_destroy(pmedata);
    }

    // Ewald method

    else if (ewald) {

        // setup reciprocal box

        float recipBoxSize[3] = { TWO_PI / periodicBoxSize[0], TWO_PI / periodicBoxSize[1], TWO_PI / periodicBoxSize[2]};


        // setup K-vectors

        #define EIR(x, y, z) eir[(x)*numberOfAtoms*3+(y)*3+z]
        vector<d_complex> eir(kmax*numberOfAtoms*3);
        vector<d_complex> tab_xy(numberOfAtoms);
        vector<d_complex> tab_qxyz(numberOfAtoms);

        for (int i = 0; (i < numberOfAtoms); i++) {
            float* pos = posq+4*i;
            for (int m = 0; (m < 3); m++)
              EIR(0, i, m) = d_complex(1,0);

            for (int m=0; (m<3); m++)
              EIR(1, i, m) = d_complex(cos(pos[m]*recipBoxSize[m]),
                                       sin(pos[m]*recipBoxSize[m]));

            for (int j=2; (j<kmax); j++)
              for (int m=0; (m<3); m++)
                EIR(j, i, m) = EIR(j-1, i, m) * EIR(1, i, m);
        }

        // calculate reciprocal space energy and forces

        int lowry = 0;
        int lowrz = 1;

        for (int rx = 0; rx < numRx; rx++) {
            float kx = rx * recipBoxSize[0];
            for (int ry = lowry; ry < numRy; ry++) {
                float ky = ry * recipBoxSize[1];
                if (ry >= 0) {
                    for (int n = 0; n < numberOfAtoms; n++)
                      tab_xy[n] = EIR(rx, n, 0) * EIR(ry, n, 1);
                }
                else {
                    for (int n = 0; n < numberOfAtoms; n++)
                      tab_xy[n]= EIR(rx, n, 0) * conj (EIR(-ry, n, 1));
                }
                for (int rz = lowrz; rz < numRz; rz++) {
                    if (rz >= 0) {
                        for (int n = 0; n < numberOfAtoms; n++)
                            tab_qxyz[n] = posq[4*n+3] * (tab_xy[n] * EIR(rz, n, 2));
                    }
                    else {
                        for (int n = 0; n < numberOfAtoms; n++)
                            tab_qxyz[n] = posq[4*n+3] * (tab_xy[n] * conj(EIR(-rz, n, 2)));
                    }
                    float cs = 0.0f;
                    float ss = 0.0f;

                    for (int n = 0; n < numberOfAtoms; n++) {
                        cs += tab_qxyz[n].real();
                        ss += tab_qxyz[n].imag();
                    }

                    float kz = rz * recipBoxSize[2];
                    float k2 = kx * kx + ky * ky + kz * kz;
                    float ak = exp(k2*factorEwald) / k2;

                    for (int n = 0; n < numberOfAtoms; n++) {
                        float force = ak * (cs * tab_qxyz[n].imag() - ss * tab_qxyz[n].real());
                        forces[n][0] += 2 * recipCoeff * force * kx;
                        forces[n][1] += 2 * recipCoeff * force * ky;
                        forces[n][2] += 2 * recipCoeff * force * kz;
                    }

                    if (totalEnergy)
                        *totalEnergy += recipCoeff * ak * (cs * cs + ss * ss);

                    lowrz = 1 - numRz;
                }
                lowry = 1 - numRy;
            }
        }
    }
}


void CpuNonbondedForce::calculateDirectIxn(int numberOfAtoms, float* posq, const vector<RealVec>& atomCoordinates, const vector<pair<float, float> >& atomParameters,
                const vector<set<int> >& exclusions, float* forces, float* totalEnergy, ThreadPool& threads) {
    // Record the parameters for the threads.
    
    this->numberOfAtoms = numberOfAtoms;
    this->posq = posq;
    this->atomCoordinates = &atomCoordinates[0];
    this->atomParameters = &atomParameters[0];
    this->exclusions = &exclusions[0];
    includeEnergy = (totalEnergy != NULL);
    threadEnergy.resize(threads.getNumThreads());
    threadForce.resize(threads.getNumThreads());
    
    // Signal the threads to start running and wait for them to finish.
    
    ComputeDirectTask task(*this);
    threads.execute(task);
    threads.waitForThreads();
    
    // Combine the results from all the threads.
    
    double directEnergy = 0;
    int numThreads = threads.getNumThreads();
    for (int i = 0; i < numThreads; i++)
        directEnergy += threadEnergy[i];
    for (int i = 0; i < numberOfAtoms; i++) {
        fvec4 f(forces+4*i);
        for (int j = 0; j < numThreads; j++)
            f += fvec4(&threadForce[j][4*i]);
        f.store(forces+4*i);
    }

    if (totalEnergy != NULL)
        *totalEnergy += (float) directEnergy;
}

void CpuNonbondedForce::threadComputeDirect(ThreadPool& threads, int threadIndex) {
    // Compute this thread's subset of interactions.

    int numThreads = threads.getNumThreads();
    threadEnergy[threadIndex] = 0;
    double* energyPtr = (includeEnergy ? &threadEnergy[threadIndex] : NULL);
    threadForce[threadIndex].resize(4*numberOfAtoms, 0.0f);
    float* forces = &threadForce[threadIndex][0];
    for (int i = 0; i < 4*numberOfAtoms; i++)
        forces[i] = 0.0f;
    fvec4 boxSize(periodicBoxSize[0], periodicBoxSize[1], periodicBoxSize[2], 0);
    fvec4 invBoxSize((1/periodicBoxSize[0]), (1/periodicBoxSize[1]), (1/periodicBoxSize[2]), 0);
    if (ewald || pme) {
        // Compute the interactions from the neighbor list.

        for (int i = threadIndex; i < neighborList->getNumBlocks(); i += numThreads)
            calculateBlockEwaldIxn(i, forces, energyPtr, boxSize, invBoxSize);

        // Now subtract off the exclusions, since they were implicitly included in the reciprocal space sum.

        fvec4 boxSize(periodicBoxSize[0], periodicBoxSize[1], periodicBoxSize[2], 0);
        fvec4 invBoxSize((1/periodicBoxSize[0]), (1/periodicBoxSize[1]), (1/periodicBoxSize[2]), 0);
        for (int i = threadIndex; i < numberOfAtoms; i += numThreads) {
            fvec4 posI((float) atomCoordinates[i][0], (float) atomCoordinates[i][1], (float) atomCoordinates[i][2], 0.0f);
            for (set<int>::const_iterator iter = exclusions[i].begin(); iter != exclusions[i].end(); ++iter) {
                if (*iter > i) {
                    int j = *iter;
                    fvec4 deltaR;
                    fvec4 posJ((float) atomCoordinates[j][0], (float) atomCoordinates[j][1], (float) atomCoordinates[j][2], 0.0f);
                    float r2;
                    getDeltaR(posJ, posI, deltaR, r2, false, boxSize, invBoxSize);
                    float r = sqrtf(r2);
                    float inverseR = 1/r;
                    float chargeProd = ONE_4PI_EPS0*posq[4*i+3]*posq[4*j+3];
                    float alphaR = alphaEwald*r;
                    float erfcAlphaR = erfcApprox(alphaR)[0];
                    float dEdR = (float) (chargeProd * inverseR * inverseR * inverseR);
                    dEdR = (float) (dEdR * (1.0f-erfcAlphaR-TWO_OVER_SQRT_PI*alphaR*exp(-alphaR*alphaR)));
                    fvec4 result = deltaR*dEdR;
                    (fvec4(forces+4*i)-result).store(forces+4*i);
                    (fvec4(forces+4*j)+result).store(forces+4*j);
                    if (includeEnergy)
                        threadEnergy[threadIndex] -= chargeProd*inverseR*(1.0f-erfcAlphaR);
                }
            }
        }
    }
    else if (cutoff) {
        // Compute the interactions from the neighbor list.

        for (int i = threadIndex; i < neighborList->getNumBlocks(); i += numThreads)
            calculateBlockIxn(i, forces, energyPtr, boxSize, invBoxSize);
    }
    else {
        // Loop over all atom pairs

        for (int i = threadIndex; i < numberOfAtoms; i += numThreads){
            for (int j = i+1; j < numberOfAtoms; j++)
                if (exclusions[j].find(i) == exclusions[j].end())
                    calculateOneIxn(i, j, forces, energyPtr, boxSize, invBoxSize);
        }
    }
}

void CpuNonbondedForce::calculateOneIxn(int ii, int jj, float* forces, double* totalEnergy, const fvec4& boxSize, const fvec4& invBoxSize) {
    // get deltaR, R2, and R between 2 atoms

    fvec4 deltaR;
    fvec4 posI(posq+4*ii);
    fvec4 posJ(posq+4*jj);
    float r2;
    getDeltaR(posJ, posI, deltaR, r2, periodic, boxSize, invBoxSize);
    if (cutoff && r2 >= cutoffDistance*cutoffDistance)
        return;
    float r = sqrtf(r2);
    float inverseR = 1/r;
    float switchValue = 1, switchDeriv = 0;
    if (useSwitch && r > switchingDistance) {
        float t = (r-switchingDistance)/(cutoffDistance-switchingDistance);
        switchValue = 1+t*t*t*(-10+t*(15-t*6));
        switchDeriv = t*t*(-30+t*(60-t*30))/(cutoffDistance-switchingDistance);
    }
    float sig       = atomParameters[ii].first + atomParameters[jj].first;
    float sig2      = inverseR*sig;
          sig2     *= sig2;
    float sig6      = sig2*sig2*sig2;

    float eps       = atomParameters[ii].second*atomParameters[jj].second;
    float dEdR      = switchValue*eps*(12.0f*sig6 - 6.0f)*sig6;
    float chargeProd = ONE_4PI_EPS0*posq[4*ii+3]*posq[4*jj+3];
    if (cutoff)
        dEdR += (float) (chargeProd*(inverseR-2.0f*krf*r2));
    else
        dEdR += (float) (chargeProd*inverseR);
    dEdR *= inverseR*inverseR;
    float energy = eps*(sig6-1.0f)*sig6;
    if (useSwitch) {
        dEdR -= energy*switchDeriv*inverseR;
        energy *= switchValue;
    }

    // accumulate energies

    if (totalEnergy) {
        if (cutoff)
            energy += (float) (chargeProd*(inverseR+krf*r2-crf));
        else
            energy += (float) (chargeProd*inverseR);
        *totalEnergy += energy;
    }

    // accumulate forces

    fvec4 result = deltaR*dEdR;
    (fvec4(forces+4*ii)+result).store(forces+4*ii);
    (fvec4(forces+4*jj)-result).store(forces+4*jj);
  }

void CpuNonbondedForce::calculateBlockIxn(int blockIndex, float* forces, double* totalEnergy, const fvec4& boxSize, const fvec4& invBoxSize) {
    // Load the positions and parameters of the atoms in the block.
    
    int blockAtom[4];
    fvec4 blockAtomPosq[4];
    fvec4 blockAtomForce[4];
    for (int i = 0; i < 4; i++) {
        blockAtom[i] = neighborList->getSortedAtoms()[4*blockIndex+i];
        blockAtomPosq[i] = fvec4(posq+4*blockAtom[i]);
        blockAtomForce[i] = fvec4(0.0f);
    }
    fvec4 blockAtomX = fvec4(blockAtomPosq[0][0], blockAtomPosq[1][0], blockAtomPosq[2][0], blockAtomPosq[3][0]);
    fvec4 blockAtomY = fvec4(blockAtomPosq[0][1], blockAtomPosq[1][1], blockAtomPosq[2][1], blockAtomPosq[3][1]);
    fvec4 blockAtomZ = fvec4(blockAtomPosq[0][2], blockAtomPosq[1][2], blockAtomPosq[2][2], blockAtomPosq[3][2]);
    fvec4 blockAtomCharge = fvec4(ONE_4PI_EPS0)*fvec4(blockAtomPosq[0][3], blockAtomPosq[1][3], blockAtomPosq[2][3], blockAtomPosq[3][3]);
    fvec4 blockAtomSigma(atomParameters[blockAtom[0]].first, atomParameters[blockAtom[1]].first, atomParameters[blockAtom[2]].first, atomParameters[blockAtom[3]].first);
    fvec4 blockAtomEpsilon(atomParameters[blockAtom[0]].second, atomParameters[blockAtom[1]].second, atomParameters[blockAtom[2]].second, atomParameters[blockAtom[3]].second);
    bool needPeriodic = false;
    if (periodic) {
        for (int i = 0; i < 4 && !needPeriodic; i++)
            for (int j = 0; j < 3; j++)
                if (blockAtomPosq[i][j]-cutoffDistance < 0.0 || blockAtomPosq[i][j]+cutoffDistance > boxSize[j]) {
                    needPeriodic = true;
                    break;
                }
    }
    
    // Loop over neighbors for this block.
    
    const vector<int>& neighbors = neighborList->getBlockNeighbors(blockIndex);
    const vector<char>& exclusions = neighborList->getBlockExclusions(blockIndex);
    bool include[4];
    for (int i = 0; i < (int) neighbors.size(); i++) {
        // Load the next neighbor.
        
        int atom = neighbors[i];
        fvec4 atomPosq(posq+4*atom);
        
        // Compute the distances to the block atoms.
        
        bool any = false;
        fvec4 dx, dy, dz, r2;
        getDeltaR(atomPosq, blockAtomX, blockAtomY, blockAtomZ, dx, dy, dz, r2, needPeriodic, boxSize, invBoxSize);
        for (int j = 0; j < 4; j++) {
            include[j] = (((exclusions[i]>>j)&1) == 0 && (!cutoff || r2[j] < cutoffDistance*cutoffDistance));
            any |= include[j];
        }
        if (!any)
            continue; // No interactions to compute.
        
        // Compute the interactions.
        
        fvec4 r = sqrt(r2);
        fvec4 inverseR = fvec4(1.0f)/r;
        fvec4 switchValue(1.0f), switchDeriv(0.0f);
        if (useSwitch) {
            fvec4 t = (r>switchingDistance) & ((r-switchingDistance)/(cutoffDistance-switchingDistance));
            switchValue = 1+t*t*t*(-10.0f+t*(15.0f-t*6.0f));
            switchDeriv = t*t*(-30.0f+t*(60.0f-t*30.0f))/(cutoffDistance-switchingDistance);
        }
        fvec4 sig = blockAtomSigma+atomParameters[atom].first;
        fvec4 sig2 = inverseR*sig;
        sig2 *= sig2;
        fvec4 sig6 = sig2*sig2*sig2;
        fvec4 eps = blockAtomEpsilon*atomParameters[atom].second;
        fvec4 dEdR = switchValue*eps*(12.0f*sig6 - 6.0f)*sig6;
        fvec4 chargeProd = blockAtomCharge*posq[4*atom+3];
        if (cutoff)
            dEdR += chargeProd*(inverseR-2.0f*krf*r2);
        else
            dEdR += chargeProd*inverseR;
        dEdR *= inverseR*inverseR;
        fvec4 energy = eps*(sig6-1.0f)*sig6;
        if (useSwitch) {
            dEdR -= energy*switchDeriv*inverseR;
            energy *= switchValue;
        }

        // Accumulate energies.

        if (totalEnergy) {
            if (cutoff)
                energy += chargeProd*(inverseR+krf*r2-crf);
            else
                energy += chargeProd*inverseR;
            for (int j = 0; j < 4; j++)
                if (include[j])
                    *totalEnergy += energy[j];
        }

        // Accumulate forces.

        fvec4 result[4] = {dx*dEdR, dy*dEdR, dz*dEdR, 0.0f};
        transpose(result[0], result[1], result[2], result[3]);
        fvec4 atomForce(forces+4*atom);
        for (int j = 0; j < 4; j++) {
            if (include[j]) {
                blockAtomForce[j] += result[j];
                atomForce -= result[j];
            }
        }
        atomForce.store(forces+4*atom);
    }
    
    // Record the forces on the block atoms.
    
    for (int j = 0; j < 4; j++)
        (fvec4(forces+4*blockAtom[j])+blockAtomForce[j]).store(forces+4*blockAtom[j]);
  }

void CpuNonbondedForce::calculateBlockEwaldIxn(int blockIndex, float* forces, double* totalEnergy, const fvec4& boxSize, const fvec4& invBoxSize) {
    // Load the positions and parameters of the atoms in the block.
    
    int blockAtom[4];
    fvec4 blockAtomPosq[4];
    fvec4 blockAtomForce[4];
    for (int i = 0; i < 4; i++) {
        blockAtom[i] = neighborList->getSortedAtoms()[4*blockIndex+i];
        blockAtomPosq[i] = fvec4(posq+4*blockAtom[i]);
        blockAtomForce[i] = fvec4(0.0f);
    }
    fvec4 blockAtomX = fvec4(blockAtomPosq[0][0], blockAtomPosq[1][0], blockAtomPosq[2][0], blockAtomPosq[3][0]);
    fvec4 blockAtomY = fvec4(blockAtomPosq[0][1], blockAtomPosq[1][1], blockAtomPosq[2][1], blockAtomPosq[3][1]);
    fvec4 blockAtomZ = fvec4(blockAtomPosq[0][2], blockAtomPosq[1][2], blockAtomPosq[2][2], blockAtomPosq[3][2]);
    fvec4 blockAtomCharge = fvec4(ONE_4PI_EPS0)*fvec4(blockAtomPosq[0][3], blockAtomPosq[1][3], blockAtomPosq[2][3], blockAtomPosq[3][3]);
    fvec4 blockAtomSigma(atomParameters[blockAtom[0]].first, atomParameters[blockAtom[1]].first, atomParameters[blockAtom[2]].first, atomParameters[blockAtom[3]].first);
    fvec4 blockAtomEpsilon(atomParameters[blockAtom[0]].second, atomParameters[blockAtom[1]].second, atomParameters[blockAtom[2]].second, atomParameters[blockAtom[3]].second);
    bool needPeriodic = false;
    for (int i = 0; i < 4 && !needPeriodic; i++)
        for (int j = 0; j < 3; j++)
            if (blockAtomPosq[i][j]-cutoffDistance < 0.0 || blockAtomPosq[i][j]+cutoffDistance > boxSize[j]) {
                needPeriodic = true;
                break;
            }
    
    // Loop over neighbors for this block.
    
    const vector<int>& neighbors = neighborList->getBlockNeighbors(blockIndex);
    const vector<char>& exclusions = neighborList->getBlockExclusions(blockIndex);
    bool include[4];
    for (int i = 0; i < (int) neighbors.size(); i++) {
        // Load the next neighbor.
        
        int atom = neighbors[i];
        fvec4 atomPosq(posq+4*atom);
        
        // Compute the distances to the block atoms.
        
        bool any = false;
        fvec4 dx, dy, dz, r2;
        getDeltaR(atomPosq, blockAtomX, blockAtomY, blockAtomZ, dx, dy, dz, r2, needPeriodic, boxSize, invBoxSize);
        for (int j = 0; j < 4; j++) {
            include[j] = (((exclusions[i]>>j)&1) == 0 && r2[j] < cutoffDistance*cutoffDistance);
            any |= include[j];
        }
        if (!any)
            continue; // No interactions to compute.
        
        // Compute the interactions.
        
        fvec4 r = sqrt(r2);
        fvec4 inverseR = fvec4(1.0f)/r;
        fvec4 switchValue(1.0f), switchDeriv(0.0f);
        if (useSwitch) {
            fvec4 t = (r>switchingDistance) & ((r-switchingDistance)/(cutoffDistance-switchingDistance));
            switchValue = 1+t*t*t*(-10.0f+t*(15.0f-t*6.0f));
            switchDeriv = t*t*(-30.0f+t*(60.0f-t*30.0f))/(cutoffDistance-switchingDistance);
        }
        fvec4 chargeProd = blockAtomCharge*posq[4*atom+3];
        fvec4 dEdR = chargeProd*inverseR*ewaldScaleFunction(r);
        fvec4 sig = blockAtomSigma+atomParameters[atom].first;
        fvec4 sig2 = inverseR*sig;
        sig2 *= sig2;
        fvec4 sig6 = sig2*sig2*sig2;
        fvec4 eps = blockAtomEpsilon*atomParameters[atom].second;
        dEdR += switchValue*eps*(12.0f*sig6 - 6.0f)*sig6;
        dEdR *= inverseR*inverseR;
        fvec4 energy = eps*(sig6-1.0f)*sig6;
        if (useSwitch) {
            dEdR -= energy*switchDeriv*inverseR;
            energy *= switchValue;
        }

        // Accumulate energies.

        if (totalEnergy) {
            energy += chargeProd*inverseR*erfcApprox(alphaEwald*r);
            for (int j = 0; j < 4; j++)
                if (include[j])
                    *totalEnergy += energy[j];
        }

        // Accumulate forces.

        fvec4 result[4] = {dx*dEdR, dy*dEdR, dz*dEdR, 0.0f};
        transpose(result[0], result[1], result[2], result[3]);
        fvec4 atomForce(forces+4*atom);
        for (int j = 0; j < 4; j++) {
            if (include[j]) {
                blockAtomForce[j] += result[j];
                atomForce -= result[j];
            }
        }
        atomForce.store(forces+4*atom);
    }
    
    // Record the forces on the block atoms.
    
    for (int j = 0; j < 4; j++)
        (fvec4(forces+4*blockAtom[j])+blockAtomForce[j]).store(forces+4*blockAtom[j]);
}

void CpuNonbondedForce::getDeltaR(const fvec4& posI, const fvec4& posJ, fvec4& deltaR, float& r2, bool periodic, const fvec4& boxSize, const fvec4& invBoxSize) const {
    deltaR = posJ-posI;
    if (periodic) {
        fvec4 base = round(deltaR*invBoxSize)*boxSize;
        deltaR = deltaR-base;
    }
    r2 = dot3(deltaR, deltaR);
}

void CpuNonbondedForce::getDeltaR(const fvec4& posI, const fvec4& x, const fvec4& y, const fvec4& z, fvec4& dx, fvec4& dy, fvec4& dz, fvec4& r2, bool periodic, const fvec4& boxSize, const fvec4& invBoxSize) const {
    dx = x-posI[0];
    dy = y-posI[1];
    dz = z-posI[2];
    if (periodic) {
        dx -= round(dx*invBoxSize[0])*boxSize[0];
        dy -= round(dy*invBoxSize[1])*boxSize[1];
        dz -= round(dz*invBoxSize[2])*boxSize[2];
    }
    r2 = dx*dx + dy*dy + dz*dz;
}

fvec4 CpuNonbondedForce::erfcApprox(fvec4 x) {
    // This approximation for erfc is from Abramowitz and Stegun (1964) p. 299.  They cite the following as
    // the original source: C. Hastings, Jr., Approximations for Digital Computers (1955).  It has a maximum
    // error of 3e-7.

    fvec4 t = 1.0f+(0.0705230784f+(0.0422820123f+(0.0092705272f+(0.0001520143f+(0.0002765672f+0.0000430638f*x)*x)*x)*x)*x)*x;
    t *= t;
    t *= t;
    t *= t;
    return 1.0f/(t*t);
}

fvec4 CpuNonbondedForce::ewaldScaleFunction(fvec4 x) {
    // Compute the tabulated Ewald scale factor: erfc(alpha*r) + 2*alpha*r*exp(-alpha*alpha*r*r)/sqrt(PI)

    float y[4];
    fvec4 x1 = x*ewaldDXInv;
    ivec4 index = floor(x1);
    fvec4 coeff[4];
    coeff[1] = x1-index;
    coeff[0] = 1.0f-coeff[1];
    coeff[2] = coeff[0]*coeff[0]*coeff[0]-coeff[0];
    coeff[3] = coeff[1]*coeff[1]*coeff[1]-coeff[1];
    transpose(coeff[0], coeff[1], coeff[2], coeff[3]);
    for (int i = 0; i < 4; i++) {
        if (index[i] < NUM_TABLE_POINTS)
            y[i] = dot4(coeff[i], fvec4(&ewaldScaleTable[4*index[i]]));
    }
    return fvec4(y);
}
