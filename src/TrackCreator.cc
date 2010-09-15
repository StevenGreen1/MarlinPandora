/**
 *  @file   PandoraPFANew/src/TrackCreator.cc
 * 
 *  @brief  Implementation of the track creator class.
 * 
 *  $Log: $
 */

#include "marlin/Global.h"
#include "marlin/Processor.h"

#include "EVENT/LCCollection.h"
#include "EVENT/ReconstructedParticle.h"
#include "EVENT/Vertex.h"

#include "gear/BField.h"
#include "gear/CalorimeterParameters.h"
#include "gear/PadRowLayout2D.h"
#include "gear/TPCParameters.h"

#include "PandoraPFANewProcessor.h"
#include "TrackCreator.h"
#include "Pandora/PdgTable.h"

#include <algorithm>
#include <cmath>
#include <limits>

TrackVector TrackCreator::m_trackVector;

//------------------------------------------------------------------------------------------------------------------------------------------

TrackCreator::TrackCreator(const Settings &settings) :
    m_settings(settings)
{
    try
    {
        m_pPandora = PandoraPFANewProcessor::GetPandora();
        m_bField = marlin::Global::GEAR->getBField().at(gear::Vector3D(0., 0., 0.)).z();

        // TPC parameters
        const gear::TPCParameters &tpcParameters(marlin::Global::GEAR->getTPCParameters());
        m_tpcInnerR = tpcParameters.getPadLayout().getPlaneExtent()[0];
        m_tpcOuterR = tpcParameters.getPadLayout().getPlaneExtent()[1];
        m_tpcZmax = tpcParameters.getMaxDriftLength();
        m_tpcMaxRow = tpcParameters.getPadLayout().getNRows();

        if ((0.f == m_tpcZmax) || (0.f == m_tpcInnerR) || (m_tpcInnerR == m_tpcOuterR))
            throw StatusCodeException(STATUS_CODE_INVALID_PARAMETER);

        m_cosTpc = m_tpcZmax / std::sqrt(m_tpcZmax * m_tpcZmax + m_tpcInnerR * m_tpcInnerR);

        // FTD parameters
        const gear::GearParameters &ftdParameters(marlin::Global::GEAR->getGearParameters("FTD"));
        m_ftdInnerRadii = ftdParameters.getDoubleVals("FTDInnerRadius");
        m_ftdOuterRadii = ftdParameters.getDoubleVals("FTDOuterRadius");
        m_ftdZPositions = ftdParameters.getDoubleVals("FTDZCoordinate");
        m_nFtdLayers = m_ftdZPositions.size();

        if ((0 == m_nFtdLayers) || (m_nFtdLayers != m_ftdInnerRadii.size()) || (m_nFtdLayers != m_ftdOuterRadii.size()))
            throw StatusCodeException(STATUS_CODE_INVALID_PARAMETER);

        for (unsigned int iFtdLayer = 0; iFtdLayer < m_nFtdLayers; ++iFtdLayer)
        {
            if ((0.f == m_ftdOuterRadii[iFtdLayer]) || (0.f == m_ftdInnerRadii[iFtdLayer]))
                throw StatusCodeException(STATUS_CODE_INVALID_PARAMETER);;
        }

        m_tanLambdaFtd = m_ftdZPositions[0] / m_ftdOuterRadii[0];

        // ETD and SET parameters
        const DoubleVector &etdZPositions(marlin::Global::GEAR->getGearParameters("ETD").getDoubleVals("ETDLayerZ"));
        const DoubleVector &setInnerRadii(marlin::Global::GEAR->getGearParameters("SET").getDoubleVals("SETLayerRadius"));

        if (etdZPositions.empty() || setInnerRadii.empty())
            throw StatusCodeException(STATUS_CODE_INVALID_PARAMETER);

        m_minEtdZPosition = *(std::min_element(etdZPositions.begin(), etdZPositions.end()));
        m_minSetRadius = *(std::min_element(setInnerRadii.begin(), setInnerRadii.end()));

        // ECal parameters
        const gear::CalorimeterParameters &ecalBarrelParameters(marlin::Global::GEAR->getEcalBarrelParameters());
        const gear::CalorimeterParameters &ecalEndCapParameters(marlin::Global::GEAR->getEcalEndcapParameters());
        m_ecalBarrelInnerSymmetry = ecalBarrelParameters.getSymmetryOrder();
        m_ecalBarrelInnerPhi0 = ecalBarrelParameters.getPhi0();
        m_ecalBarrelInnerR = ecalBarrelParameters.getExtent()[0];
        m_ecalEndCapInnerZ = ecalEndCapParameters.getExtent()[2];
    }
    catch (...)
    {
        streamlog_out(ERROR) << "Failed to initialize track creator." << std::endl;
        throw StatusCodeException(STATUS_CODE_INVALID_PARAMETER);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------

TrackCreator::~TrackCreator()
{
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode TrackCreator::CreateTrackAssociations(const LCEvent *const pLCEvent)
{
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ExtractKinks(pLCEvent));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ExtractProngsAndSplits(pLCEvent));
    PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, this->ExtractV0s(pLCEvent));

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode TrackCreator::ExtractKinks(const LCEvent *const pLCEvent)
{
    for (StringVector::const_iterator iter = m_settings.m_kinkVertexCollections.begin(), iterEnd = m_settings.m_kinkVertexCollections.end();
        iter != iterEnd; ++iter)
    {
        try
        {
            const LCCollection *pKinkCollection = pLCEvent->getCollection(*iter);

            for (int i = 0, iMax = pKinkCollection->getNumberOfElements(); i < iMax; ++i)
            {
                try
                {
                    Vertex *pVertex = dynamic_cast<Vertex*>(pKinkCollection->getElementAt(i));

                    ReconstructedParticle *pReconstructedParticle = pVertex->getAssociatedParticle();
                    const TrackVec &trackVec(pReconstructedParticle->getTracks());

                    if (this->IsConflictingRelationship(trackVec))
                        continue;

                    const int vertexPdgCode(pReconstructedParticle->getType());

                    // Extract the kink vertex information
                    for (unsigned int iTrack = 0, nTracks = trackVec.size(); iTrack < nTracks; ++iTrack)
                    {
                        Track *pTrack = trackVec[iTrack];
                        (0 == iTrack) ? m_parentTrackList.insert(pTrack) : m_daughterTrackList.insert(pTrack);
                        streamlog_out(DEBUG) << "KinkTrack " << iTrack << ", nHits " << pTrack->getTrackerHits().size() << std::endl;

                        int trackPdgCode = pandora::UNKNOWN;

                        if (0 == iTrack)
                        {
                            trackPdgCode = vertexPdgCode;
                        }
                        else
                        {
                            switch (vertexPdgCode)
                            {
                            case pandora::PI_PLUS :
                            case pandora::K_PLUS :
                                trackPdgCode = pandora::MU_PLUS;
                                break;
                            case pandora::PI_MINUS :
                            case pandora::K_MINUS :
                                trackPdgCode = pandora::MU_MINUS;
                                break;
                            case pandora::HYPERON_MINUS_BAR :
                            case pandora::SIGMA_PLUS :
                                trackPdgCode = pandora::PI_PLUS;
                                break;
                            case pandora::SIGMA_MINUS :
                            case pandora::HYPERON_MINUS :
                                trackPdgCode = pandora::PI_PLUS;
                                break;
                            default :
                                (pTrack->getOmega() > 0) ? trackPdgCode = pandora::PI_PLUS : trackPdgCode = pandora::PI_MINUS;
                                break;
                            }
                        }

                        m_trackToPidMap.insert(TrackToPidMap::value_type(pTrack, trackPdgCode));

                        if (0 == m_settings.m_shouldFormTrackRelationships)
                            continue;

                        // Make track parent-daughter relationships
                        if (0 == iTrack)
                        {
                            for (unsigned int jTrack = iTrack + 1; jTrack < nTracks; ++jTrack)
                            {
                                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::SetTrackParentDaughterRelationship(*m_pPandora,
                                    pTrack, trackVec[jTrack]));
                            }
                        }

                        // Make track sibling relationships
                        else
                        {
                            for (unsigned int jTrack = iTrack + 1; jTrack < nTracks; ++jTrack)
                            {
                                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::SetTrackSiblingRelationship(*m_pPandora,
                                    pTrack, trackVec[jTrack]));
                            }
                        }
                    }
                }
                catch (...)
                {
                    streamlog_out(WARNING) << "Failed to extract kink vertex, unrecognised exception" << std::endl;
                }
            }
        }
        catch (...)
        {
            streamlog_out(MESSAGE) << "Failed to extract kink vertex collection: " << *iter << std::endl;
        }
    }

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode TrackCreator::ExtractProngsAndSplits(const LCEvent *const pLCEvent)
{
    for (StringVector::const_iterator iter = m_settings.m_prongSplitVertexCollections.begin(), iterEnd = m_settings.m_prongSplitVertexCollections.end();
        iter != iterEnd; ++iter)
    {
        try
        {
            const LCCollection *pProngOrSplitCollection = pLCEvent->getCollection(*iter);

            for (int i = 0, iMax = pProngOrSplitCollection->getNumberOfElements(); i < iMax; ++i)
            {
                try
                {
                    Vertex *pVertex = dynamic_cast<Vertex*>(pProngOrSplitCollection->getElementAt(i));

                    ReconstructedParticle *pReconstructedParticle = pVertex->getAssociatedParticle();
                    const TrackVec &trackVec(pReconstructedParticle->getTracks());

                    if (this->IsConflictingRelationship(trackVec))
                        continue;

                    // Extract the prong/split vertex information
                    for (unsigned int iTrack = 0, nTracks = trackVec.size(); iTrack < nTracks; ++iTrack)
                    {
                        Track *pTrack = trackVec[iTrack];
                        (0 == iTrack) ? m_parentTrackList.insert(pTrack) : m_daughterTrackList.insert(pTrack);
                        streamlog_out(DEBUG) << "Prong or Split Track " << iTrack << ", nHits " << pTrack->getTrackerHits().size() << std::endl;

                        if (0 == m_settings.m_shouldFormTrackRelationships)
                            continue;

                        // Make track parent-daughter relationships
                        if (0 == iTrack)
                        {
                            for (unsigned int jTrack = iTrack + 1; jTrack < nTracks; ++jTrack)
                            {
                                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::SetTrackParentDaughterRelationship(*m_pPandora,
                                    pTrack, trackVec[jTrack]));
                            }
                        }

                        // Make track sibling relationships
                        else
                        {
                            for (unsigned int jTrack = iTrack + 1; jTrack < nTracks; ++jTrack)
                            {
                                PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::SetTrackSiblingRelationship(*m_pPandora,
                                    pTrack, trackVec[jTrack]));
                            }
                        }
                    }
                }
                catch (...)
                {
                    streamlog_out(WARNING) << "Failed to extract prong/split vertex, unrecognised exception" << std::endl;
                }
            }
        }
        catch (...)
        {
            streamlog_out(MESSAGE) << "Failed to extract prong/split vertex collection: " << *iter << std::endl;
        }
    }

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode TrackCreator::ExtractV0s(const LCEvent *const pLCEvent)
{
    for (StringVector::const_iterator iter = m_settings.m_v0VertexCollections.begin(), iterEnd = m_settings.m_v0VertexCollections.end();
        iter != iterEnd; ++iter)
    {
        try
        {
            const LCCollection *pV0Collection = pLCEvent->getCollection(*iter);

            for (int i = 0, iMax = pV0Collection->getNumberOfElements(); i < iMax; ++i)
            {
                try
                {
                    Vertex *pVertex = dynamic_cast<Vertex*>(pV0Collection->getElementAt(i));

                    ReconstructedParticle *pReconstructedParticle = pVertex->getAssociatedParticle();
                    const TrackVec &trackVec(pReconstructedParticle->getTracks());

                    if (this->IsConflictingRelationship(trackVec))
                        continue;

                    // Extract the v0 vertex information
                    const int vertexPdgCode(pReconstructedParticle->getType());

                    for (unsigned int iTrack = 0, nTracks = trackVec.size(); iTrack < nTracks; ++iTrack)
                    {
                        Track *pTrack = trackVec[iTrack];
                        m_v0TrackList.insert(pTrack);
                        streamlog_out(DEBUG) << "V0Track " << iTrack << ", nHits " << pTrack->getTrackerHits().size() << std::endl;

                        int trackPdgCode = pandora::UNKNOWN;

                        switch (vertexPdgCode)
                        {
                        case pandora::PHOTON :
                            (pTrack->getOmega() > 0) ? trackPdgCode = pandora::E_PLUS : trackPdgCode = pandora::E_MINUS;
                            break;
                        case pandora::LAMBDA :
                            (pTrack->getOmega() > 0) ? trackPdgCode = pandora::PROTON : trackPdgCode = pandora::PI_MINUS;
                            break;
                        case pandora::LAMBDA_BAR :
                            (pTrack->getOmega() > 0) ? trackPdgCode = pandora::PI_PLUS : trackPdgCode = pandora::PROTON_BAR;
                            break;
                        case pandora::K_SHORT :
                            (pTrack->getOmega() > 0) ? trackPdgCode = pandora::PI_PLUS : trackPdgCode = pandora::PI_MINUS;
                            break;
                        default :
                            (pTrack->getOmega() > 0) ? trackPdgCode = pandora::PI_PLUS : trackPdgCode = pandora::PI_MINUS;
                            break;
                        }

                        m_trackToPidMap.insert(TrackToPidMap::value_type(pTrack, trackPdgCode));

                        if (0 == m_settings.m_shouldFormTrackRelationships)
                            continue;

                        // Make track sibling relationships
                        for (unsigned int jTrack = iTrack + 1; jTrack < nTracks; ++jTrack)
                        {
                            PANDORA_RETURN_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::SetTrackSiblingRelationship(*m_pPandora,
                                pTrack, trackVec[jTrack]));
                        }
                    }
                }
                catch (...)
                {
                    streamlog_out(WARNING) << "Failed to extract v0 vertex, unrecognised exception" << std::endl;
                }
            }
        }
        catch (...)
        {
            streamlog_out(MESSAGE) << "Failed to extract v0 vertex collection: " << *iter << std::endl;
        }
    }

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

bool TrackCreator::IsConflictingRelationship(const TrackVec &trackVec) const
{
    for (unsigned int iTrack = 0, nTracks = trackVec.size(); iTrack < nTracks; ++iTrack)
    {
        Track *pTrack = trackVec[iTrack];

        if (this->IsDaughter(pTrack) || this->IsParent(pTrack) || this->IsV0(pTrack))
            return true;
    }

    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------

StatusCode TrackCreator::CreateTracks(const LCEvent *const pLCEvent) const
{
    for (StringVector::const_iterator iter = m_settings.m_trackCollections.begin(), iterEnd = m_settings.m_trackCollections.end();
        iter != iterEnd; ++iter)
    {
        try
        {
            const LCCollection *pTrackCollection = pLCEvent->getCollection(*iter);

            for (int i = 0, iMax = pTrackCollection->getNumberOfElements(); i < iMax; ++i)
            {
                try
                {
                    Track *pTrack = dynamic_cast<Track*>(pTrackCollection->getElementAt(i));

                    int minTrackHits = m_settings.m_minTrackHits;
                    const float tanLambda(std::fabs(pTrack->getTanLambda()));

                    if (tanLambda > m_tanLambdaFtd)
                    {
                        int expectedFtdHits(0);

                        for (unsigned int iFtdLayer = 0; iFtdLayer < m_nFtdLayers; ++iFtdLayer)
                        {
                            if ((tanLambda > m_ftdZPositions[iFtdLayer] / m_ftdOuterRadii[iFtdLayer]) &&
                                (tanLambda < m_ftdZPositions[iFtdLayer] / m_ftdInnerRadii[iFtdLayer]))
                            {
                                expectedFtdHits++;
                            }
                        }

                        minTrackHits = std::max(m_settings.m_minFtdTrackHits, expectedFtdHits);
                    }

                    const int nTrackHits(static_cast<int>(pTrack->getTrackerHits().size()));


                    if ((nTrackHits < minTrackHits) || (nTrackHits > m_settings.m_maxTrackHits))
                        continue;

                    // Proceed to create the pandora track
                    PandoraApi::Track::Parameters trackParameters;
                    trackParameters.m_d0 = pTrack->getD0();
                    trackParameters.m_z0 = pTrack->getZ0();
                    trackParameters.m_pParentAddress = pTrack;

                    // By default, assume tracks are charged pions
                    const float signedCurvature(pTrack->getOmega());
                    trackParameters.m_particleId = (signedCurvature > 0) ? pandora::PI_PLUS : pandora::PI_MINUS;
                    trackParameters.m_mass = pandora::PdgTable::GetParticleMass(pandora::PI_PLUS);

                    // Use particle id information from V0 and Kink finders
                    TrackToPidMap::const_iterator iter = m_trackToPidMap.find(pTrack);

                    if(iter != m_trackToPidMap.end())
                    {
                        trackParameters.m_particleId = (*iter).second;
                        trackParameters.m_mass = pandora::PdgTable::GetParticleMass((*iter).second);
                    }

                    if (0. != signedCurvature)
                        trackParameters.m_charge = static_cast<int>(signedCurvature / std::fabs(signedCurvature));

                    this->FitTrackHelices(pTrack, trackParameters);
                    this->TrackReachesECAL(pTrack, trackParameters);
                    this->DefineTrackPfoUsage(pTrack, trackParameters);

                    PANDORA_THROW_RESULT_IF(STATUS_CODE_SUCCESS, !=, PandoraApi::Track::Create(*m_pPandora, trackParameters));
                    m_trackVector.push_back(pTrack);
                }
                catch (StatusCodeException &statusCodeException)
                {
                    streamlog_out(ERROR) << "Failed to extract a track: " << statusCodeException.ToString() << std::endl;
                }
                catch (...)
                {
                    streamlog_out(WARNING) << "Failed to extract a track, unrecognised exception" << std::endl;
                }
            }
        }
        catch (...)
        {
            streamlog_out(MESSAGE) << "Failed to extract track collection: " << *iter << std::endl;
        }
    }

    return STATUS_CODE_SUCCESS;
}

//------------------------------------------------------------------------------------------------------------------------------------------

void TrackCreator::FitTrackHelices(const Track *const pTrack, PandoraApi::Track::Parameters &trackParameters) const
{
    pandora::Helix *pHelixFit = new pandora::Helix(pTrack->getPhi(), pTrack->getD0(), pTrack->getZ0(), pTrack->getOmega(), pTrack->getTanLambda(), m_bField);
    trackParameters.m_momentumAtDca = pHelixFit->GetMomentum();

    const TrackerHitVec &trackerHitvec(pTrack->getTrackerHits());
    float zMin(std::numeric_limits<float>::max()), zMax(-std::numeric_limits<float>::max());

    for (int iz = 0, nTrackHits = trackerHitvec.size(); iz < nTrackHits - 1; ++iz)
    {
        const float hitZ(trackerHitvec[iz]->getPosition()[2]);

        if (hitZ > zMax)
            zMax = hitZ;

        if (hitZ < zMin)
            zMin = hitZ;
    }

    const int signPz(std::fabs(zMin) < std::fabs(zMax) ? 1 : -1);
    const float zStart((signPz > 0) ? zMin : zMax);
    const float zEnd((signPz > 0) ? zMax : zMin);

    pandora::CartesianVector startPosition, startMomentum;
    PANDORA_THROW_RESULT_IF(STATUS_CODE_SUCCESS, !=, pHelixFit->GetPointInZ(zStart, pHelixFit->GetReferencePoint(), startPosition));
    startMomentum = pHelixFit->GetExtrapolatedMomentum(startPosition);
    trackParameters.m_trackStateAtStart = pandora::TrackState(startPosition, startMomentum);

    pandora::CartesianVector endPosition, endMomentum;
    PANDORA_THROW_RESULT_IF(STATUS_CODE_SUCCESS, !=, pHelixFit->GetPointInZ(zEnd, pHelixFit->GetReferencePoint(), endPosition));
    endMomentum = pHelixFit->GetExtrapolatedMomentum(endPosition);
    trackParameters.m_trackStateAtEnd = pandora::TrackState(endPosition, endMomentum);

    trackParameters.m_trackStateAtECal = this->GetECalProjection(pHelixFit, pHelixFit->GetReferencePoint(), signPz);

    delete pHelixFit;
}

//------------------------------------------------------------------------------------------------------------------------------------------

pandora::TrackState TrackCreator::GetECalProjection(pandora::Helix *const pHelix, const pandora::CartesianVector &referencePoint, int signPz) const
{
    // First project to endcap
    float minTime(std::numeric_limits<float>::max());
    pandora::CartesianVector bestECalProjection;
    (void) pHelix->GetPointInZ(static_cast<float>(signPz) * m_ecalEndCapInnerZ, referencePoint, bestECalProjection, minTime);

    // Then project to barrel surface(s)
    static const float pi(std::acos(-1.));
    pandora::CartesianVector barrelProjection;

    if (m_ecalBarrelInnerSymmetry > 0)
    {
        // Polygon
        float twopi_n = 2. * pi / (static_cast<float>(m_ecalBarrelInnerSymmetry));

        for (int i = 0; i < m_ecalBarrelInnerSymmetry; ++i)
        {
            float time(std::numeric_limits<float>::max());
            const float phi(twopi_n * static_cast<float>(i) + m_ecalBarrelInnerPhi0);

            const StatusCode statusCode(pHelix->GetPointInXY(m_ecalBarrelInnerR * std::cos(phi), m_ecalBarrelInnerR * std::sin(phi),
                std::cos(phi + 0.5 * pi), std::sin(phi + 0.5 * pi), referencePoint, barrelProjection, time));

            if ((STATUS_CODE_SUCCESS == statusCode) && (time < minTime))
            {
                minTime = time;
                bestECalProjection = barrelProjection;
            }
        }
    }
    else
    {
        // Cylinder
        float time(std::numeric_limits<float>::max());
        const StatusCode statusCode(pHelix->GetPointOnCircle(m_ecalBarrelInnerR, referencePoint, barrelProjection, time));

        if ((STATUS_CODE_SUCCESS == statusCode) && (time < minTime))
        {
            minTime = time;
            bestECalProjection = barrelProjection;
        }
    }

    if (!bestECalProjection.IsInitialized())
        throw StatusCodeException(STATUS_CODE_NOT_INITIALIZED);

    return pandora::TrackState(bestECalProjection, pHelix->GetExtrapolatedMomentum(bestECalProjection));
}

//------------------------------------------------------------------------------------------------------------------------------------------

void TrackCreator::TrackReachesECAL(const Track *const pTrack, PandoraApi::Track::Parameters &trackParameters) const
{
    // Calculate hit position information
    float hitZMin(std::numeric_limits<float>::max());
    float hitZMax(-std::numeric_limits<float>::max());
    float hitOuterR(-std::numeric_limits<float>::max());

    int nTpcHits(0);
    int nFtdHits(0);

    const TrackerHitVec &trackerHitVec(pTrack->getTrackerHits());
    const unsigned int nTrackHits(trackerHitVec.size());

    for (unsigned int i = 0; i < nTrackHits; ++i)
    {
        const float x(static_cast<float>(trackerHitVec[i]->getPosition()[0]));
        const float y(static_cast<float>(trackerHitVec[i]->getPosition()[1]));
        const float z(static_cast<float>(trackerHitVec[i]->getPosition()[2]));
        const float r(std::sqrt(x * x + y * y));

        if (z > hitZMax)
            hitZMax = z;

        if (z < hitZMin)
            hitZMin = z;

        if (r > hitOuterR)
            hitOuterR = r;

        if ((r > m_tpcInnerR) && (r < m_tpcOuterR) && (std::fabs(z) <= m_tpcZmax))
        {
            nTpcHits++;
            continue;
        }

        for (unsigned int j = 0; j < m_nFtdLayers; ++j)
        {
            if ((r > m_ftdInnerRadii[j]) && (r < m_ftdOuterRadii[j]) &&
                (std::fabs(z) - m_settings.m_reachesECalFtdZMaxDistance < m_ftdZPositions[j]) &&
                (std::fabs(z) + m_settings.m_reachesECalFtdZMaxDistance > m_ftdZPositions[j]))
            {
                nFtdHits++;
                break;
            }
        }
    }

    // Look to see if there are hits in etd or set, implying track has reached edge of ecal
    if ((hitOuterR > m_minSetRadius) || (hitZMax > m_minEtdZPosition))
    {
        trackParameters.m_reachesECal = true;
        return;
    }

    // Require sufficient hits in tpc or ftd, then compare extremal hit positions with tracker dimensions
    if ((nTpcHits >= m_settings.m_reachesECalNTpcHits) || (nFtdHits >= m_settings.m_reachesECalNFtdHits))
    {
        if ((hitOuterR - m_tpcOuterR > m_settings.m_reachesECalTpcOuterDistance) ||
            (std::fabs(hitZMax) - m_tpcZmax > m_settings.m_reachesECalTpcZMaxDistance) ||
            (std::fabs(hitZMin) - m_tpcZmax > m_settings.m_reachesECalTpcZMaxDistance))
        {
            trackParameters.m_reachesECal = true;
            return;
        }
    }

    // If track is lowpt, it may curl up and end inside tpc inner radius
    const pandora::CartesianVector &momentumAtDca(trackParameters.m_momentumAtDca.Get());
    const float cosAngleAtDca(std::fabs(momentumAtDca.GetZ()) / momentumAtDca.GetMagnitude());
    const float pX(momentumAtDca.GetX()), pY(momentumAtDca.GetY());
    const float pT(std::sqrt(pX * pX + pY * pY));

    if ((cosAngleAtDca > m_cosTpc) || (pT < m_settings.m_curvatureToMomentumFactor * m_bField * m_tpcOuterR))
    {
        trackParameters.m_reachesECal = true;
        return;
    }

    trackParameters.m_reachesECal = false;
}

//------------------------------------------------------------------------------------------------------------------------------------------

void TrackCreator::DefineTrackPfoUsage(const Track *const pTrack, PandoraApi::Track::Parameters &trackParameters) const
{
    bool canFormPfo(false);
    bool canFormClusterlessPfo(false);

    if (trackParameters.m_reachesECal.Get() && !this->IsParent(pTrack))
    {
        const float d0(std::fabs(pTrack->getD0())), z0(std::fabs(pTrack->getZ0()));

        TrackerHitVec trackerHitvec(pTrack->getTrackerHits());
        float rInner(std::numeric_limits<float>::max()), zMin(std::numeric_limits<float>::max());

        for (TrackerHitVec::const_iterator iter = trackerHitvec.begin(), iterEnd = trackerHitvec.end(); iter != iterEnd; ++iter)
        {
            const double *pPosition((*iter)->getPosition());
            const float x(pPosition[0]), y(pPosition[1]), absoluteZ(std::fabs(pPosition[2]));
            const float r(std::sqrt(x * x + y * y));

            if (r < rInner)
                rInner = r;

            if (absoluteZ < zMin)
                zMin = absoluteZ;
        }

        if (this->PassesQualityCuts(pTrack, trackParameters, rInner))
        {
            const pandora::CartesianVector &momentumAtDca(trackParameters.m_momentumAtDca.Get());
            const float pX(momentumAtDca.GetX()), pY(momentumAtDca.GetY()), pZ(momentumAtDca.GetZ());
            const float pT(std::sqrt(pX * pX + pY * pY));

            const float zCutForNonVertexTracks(m_tpcInnerR * std::fabs(pZ / pT) + m_settings.m_zCutForNonVertexTracks);
            const bool passRzQualityCuts((zMin < zCutForNonVertexTracks) && (rInner < m_tpcInnerR + m_settings.m_maxTpcInnerRDistance));

            const bool isV0(this->IsV0(pTrack));
            const bool isDaughter(this->IsDaughter(pTrack));

            // Decide whether track can be associated with a pandora cluster and used to form a charged PFO
            if ((d0 < m_settings.m_d0TrackCut) && (z0 < m_settings.m_z0TrackCut) && (rInner < m_tpcInnerR + m_settings.m_maxTpcInnerRDistance))
            {
                canFormPfo = true;
            }
            else if (passRzQualityCuts && (0 != m_settings.m_usingNonVertexTracks))
            {
                canFormPfo = true;
            }
            else if (isV0 || isDaughter)
            {
                canFormPfo = true;
            }

            // Decide whether track can be used to form a charged PFO, even if track fails to be associated with a pandora cluster
            const float particleMass(trackParameters.m_mass.Get());
            const float trackEnergy(std::sqrt(momentumAtDca.GetMagnitudeSquared() + particleMass * particleMass));

            if ((0 != m_settings.m_usingUnmatchedVertexTracks) && (trackEnergy < m_settings.m_unmatchedVertexTrackMaxEnergy))
            {
                if ((d0 < m_settings.m_d0UnmatchedVertexTrackCut) && (z0 < m_settings.m_z0UnmatchedVertexTrackCut) &&
                    (rInner < m_tpcInnerR + m_settings.m_maxTpcInnerRDistance))
                {
                    canFormClusterlessPfo = true;
                }
                else if (passRzQualityCuts && (0 != m_settings.m_usingNonVertexTracks) && (0 != m_settings.m_usingUnmatchedNonVertexTracks))
                {
                    canFormClusterlessPfo = true;
                }
                else if (isV0 || isDaughter)
                {
                    canFormClusterlessPfo = true;
                }
            }
        }
    }

    trackParameters.m_canFormPfo = canFormPfo;
    trackParameters.m_canFormClusterlessPfo = canFormClusterlessPfo;
}

//------------------------------------------------------------------------------------------------------------------------------------------

bool TrackCreator::PassesQualityCuts(const Track *const pTrack, const PandoraApi::Track::Parameters &trackParameters, const float rInner) const
{
    // First simple sanity checks
    if (trackParameters.m_trackStateAtECal.Get().GetPosition().GetMagnitude() < m_settings.m_minTrackECalDistanceFromIp)
        return false;

    if (pTrack->getOmega() == 0.f)
    {
        streamlog_out(ERROR) << "Track has Omega = 0 " << std::endl;
        return false;
    }

    // Require reasonable number of TPC hits 
    const pandora::CartesianVector &momentumAtDca(trackParameters.m_momentumAtDca.Get());
    const float pX(fabs(momentumAtDca.GetX()));
    const float pY(fabs(momentumAtDca.GetY()));
    const float pZ(fabs(momentumAtDca.GetZ()));
    const float pT(std::sqrt(pX * pX + pY * pY));
    const float rInnermostHit(pTrack->getRadiusOfInnermostHit());

    if ((0.f == pT) || (0.f == pZ) || (rInnermostHit == m_tpcOuterR))
    {
        streamlog_out(ERROR) << "Invalid track parameter, pT " << pT << ", pZ " << pZ << ", rInnermostHit " << rInnermostHit << std::endl;
        return false;
    }

    float nExpectedTpcHits(0.);

    if (pZ < m_tpcZmax / m_tpcOuterR * pT)
    {
        const float innerExpectedHitRadius(std::max(m_tpcInnerR, rInnermostHit));
        const float frac((m_tpcOuterR - innerExpectedHitRadius) / (m_tpcOuterR - m_tpcInnerR));
        nExpectedTpcHits = m_tpcMaxRow * frac;
    }

    if ((pZ <= m_tpcZmax / m_tpcInnerR * pT) && (pZ >= m_tpcZmax / m_tpcOuterR * pT))
    {
        const float innerExpectedHitRadius(std::max(m_tpcInnerR, rInnermostHit));
        const float frac((m_tpcZmax * pT / pZ - innerExpectedHitRadius) / (m_tpcOuterR - innerExpectedHitRadius));
        nExpectedTpcHits = frac * m_tpcMaxRow;
    }

    // Account for central TPC membrane (not specified in GEAR?) take to be 10mm for now
    // TODO get information from geometry and calculate correct expected number of hits
    static const float tpcMembrane(10.f);

    if (std::fabs(pZ) / momentumAtDca.GetMagnitude() < tpcMembrane / m_tpcInnerR)
        nExpectedTpcHits = 0;

    const EVENT::IntVec &hitsBySubdetector(pTrack->getSubdetectorHitNumbers());
    const int nTpcHits = hitsBySubdetector[9];
    const int nFtdHits = hitsBySubdetector[7];
    const int minTpcHits = static_cast<int>(nExpectedTpcHits * m_settings.m_minTpcHitFractionOfExpected);

    if ((nTpcHits < minTpcHits) && (momentumAtDca.GetMagnitude() > 1.f) && (nFtdHits < m_settings.m_minFtdHitsForTpcHitFraction))
    {
        streamlog_out(WARNING) << " Dropping track : " << momentumAtDca.GetMagnitude() << " Number of TPC hits = " << nTpcHits
                               << " < " << minTpcHits << " nftd = " << nFtdHits  << std::endl;
        return false;
    }

    // Check momentum uncertainty is reasonable to use track
    const float sigmaPOverP(std::sqrt(pTrack->getCovMatrix()[5]) / std::fabs(pTrack->getOmega()));

    if (sigmaPOverP > m_settings.m_maxTrackSigmaPOverP)
    {
        const TrackerHitVec &trackerHitVec(pTrack->getTrackerHits());
        const pandora::CartesianVector &momentumAtDca(trackParameters.m_momentumAtDca.Get());

        streamlog_out(WARNING) << " Dropping track : " << momentumAtDca.GetMagnitude() << "+-" << sigmaPOverP*(momentumAtDca.GetMagnitude())
                               << " chi2 = " <<  pTrack->getChi2() << " " << pTrack->getNdf() << " from " << trackerHitVec.size() << std::endl;
        return false;
    }

    return true;
}
