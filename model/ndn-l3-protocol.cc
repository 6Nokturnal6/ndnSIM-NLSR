/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 *         Ilya Moiseenko <iliamo@cs.ucla.edu>
 */

#include "ndn-l3-protocol.h"

#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/log.h"
#include "ns3/callback.h"
#include "ns3/uinteger.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/object-vector.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/random-variable.h"

#include "ns3/ndn-header-helper.h"
#include "ns3/ndn-pit.h"
#include "ns3/ndn-interest-header.h"
#include "ns3/ndn-content-object-header.h"

#include "ns3/ndn-face.h"
#include "ns3/ndn-forwarding-strategy.h"

// #include "fib/ndn-fib-impl.h"

#include "ndn-net-device-face.h"

#include <boost/foreach.hpp>

NS_LOG_COMPONENT_DEFINE ("NdnL3Protocol");

namespace ns3 {

const uint16_t NdnL3Protocol::ETHERNET_FRAME_TYPE = 0x7777;


NS_OBJECT_ENSURE_REGISTERED (NdnL3Protocol);

TypeId 
NdnL3Protocol::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::NdnL3Protocol")
    .SetParent<Ndn> ()
    .SetGroupName ("ndn")
    .AddConstructor<NdnL3Protocol> ()
    .AddAttribute ("FaceList", "List of faces associated with ndn stack",
                   ObjectVectorValue (),
                   MakeObjectVectorAccessor (&NdnL3Protocol::m_faces),
                   MakeObjectVectorChecker<NdnFace> ())
  ;
  return tid;
}

NdnL3Protocol::NdnL3Protocol()
: m_faceCounter (0)
{
  NS_LOG_FUNCTION (this);
}

NdnL3Protocol::~NdnL3Protocol ()
{
  NS_LOG_FUNCTION (this);
}

/*
 * This method is called by AddAgregate and completes the aggregation
 * by setting the node in the ndn stack
 */
void
NdnL3Protocol::NotifyNewAggregate ()
{
  // not really efficient, but this will work only once
  if (m_node == 0)
    {
      m_node = GetObject<Node> ();
      if (m_node != 0)
        {
          // NS_ASSERT_MSG (m_pit != 0 && m_fib != 0 && m_contentStore != 0 && m_forwardingStrategy != 0,
          //                "PIT, FIB, and ContentStore should be aggregated before NdnL3Protocol");
          NS_ASSERT_MSG (m_forwardingStrategy != 0,
                         "Forwarding strategy should be aggregated before NdnL3Protocol");
        }
    }
  // if (m_pit == 0)
  //   {
  //     m_pit = GetObject<NdnPit> ();
  //   }
  // if (m_fib == 0)
  //   {
  //     m_fib = GetObject<NdnFib> ();
  //   }
  if (m_forwardingStrategy == 0)
    {
      m_forwardingStrategy = GetObject<NdnForwardingStrategy> ();
    }
  // if (m_contentStore == 0)
  //   {
  //     m_contentStore = GetObject<NdnContentStore> ();
  //   }

  Ndn::NotifyNewAggregate ();
}

void 
NdnL3Protocol::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  for (NdnFaceList::iterator i = m_faces.begin (); i != m_faces.end (); ++i)
    {
      *i = 0;
    }
  m_faces.clear ();
  m_node = 0;

  // Force delete on objects
  m_forwardingStrategy = 0; // there is a reference to PIT stored in here

  Ndn::DoDispose ();
}

uint32_t 
NdnL3Protocol::AddFace (const Ptr<NdnFace> &face)
{
  NS_LOG_FUNCTION (this << &face);

  face->SetId (m_faceCounter); // sets a unique ID of the face. This ID serves only informational purposes

  // ask face to register in lower-layer stack
  face->RegisterProtocolHandler (MakeCallback (&NdnL3Protocol::Receive, this));

  m_faces.push_back (face);
  m_faceCounter++;
  return face->GetId ();
}

void
NdnL3Protocol::RemoveFace (Ptr<NdnFace> face)
{
  // ask face to register in lower-layer stack
  face->RegisterProtocolHandler (MakeNullCallback<void,const Ptr<NdnFace>&,const Ptr<const Packet>&> ());
  Ptr<NdnPit> pit = GetObject<NdnPit> ();

  // just to be on a safe side. Do the process in two steps
  std::list< Ptr<NdnPitEntry> > entriesToRemoves;
  for (Ptr<NdnPitEntry> pitEntry = pit->Begin (); pitEntry != 0; pitEntry = pit->Next (pitEntry))
    {
      pitEntry->RemoveAllReferencesToFace (face);
      
      // If this face is the only for the associated FIB entry, then FIB entry will be removed soon.
      // Thus, we have to remove the whole PIT entry
      if (pitEntry->GetFibEntry ()->m_faces.size () == 1 &&
          pitEntry->GetFibEntry ()->m_faces.begin ()->m_face == face)
        {
          entriesToRemoves.push_back (pitEntry);
        }
    }
  BOOST_FOREACH (Ptr<NdnPitEntry> removedEntry, entriesToRemoves)
    {
      pit->MarkErased (removedEntry);
    }

  NdnFaceList::iterator face_it = find (m_faces.begin(), m_faces.end(), face);
  NS_ASSERT_MSG (face_it != m_faces.end (), "Attempt to remove face that doesn't exist");
  m_faces.erase (face_it);
}

Ptr<NdnFace>
NdnL3Protocol::GetFace (uint32_t index) const
{
  BOOST_FOREACH (const Ptr<NdnFace> &face, m_faces) // this function is not supposed to be called often, so linear search is fine
    {
      if (face->GetId () == index)
        return face;
    }
  return 0;
}

Ptr<NdnFace>
NdnL3Protocol::GetFaceByNetDevice (Ptr<NetDevice> netDevice) const
{
  BOOST_FOREACH (const Ptr<NdnFace> &face, m_faces) // this function is not supposed to be called often, so linear search is fine
    {
      Ptr<NdnNetDeviceFace> netDeviceFace = DynamicCast<NdnNetDeviceFace> (face);
      if (netDeviceFace == 0) continue;

      if (netDeviceFace->GetNetDevice () == netDevice)
        return face;
    }
  return 0;
}

uint32_t 
NdnL3Protocol::GetNFaces (void) const
{
  return m_faces.size ();
}

// Callback from lower layer
void 
NdnL3Protocol::Receive (const Ptr<NdnFace> &face, const Ptr<const Packet> &p)
{
  if (!face->IsUp ())
    return;

  NS_LOG_DEBUG (*p);
  
  NS_LOG_LOGIC ("Packet from face " << *face << " received on node " <<  m_node->GetId ());

  Ptr<Packet> packet = p->Copy (); // give upper layers a rw copy of the packet
  try
    {
      NdnHeaderHelper::Type type = NdnHeaderHelper::GetNdnHeaderType (p);
      switch (type)
        {
        case NdnHeaderHelper::INTEREST:
          {
            Ptr<NdnInterestHeader> header = Create<NdnInterestHeader> ();

            // Deserialization. Exception may be thrown
            packet->RemoveHeader (*header);
            NS_ASSERT_MSG (packet->GetSize () == 0, "Payload of Interests should be zero");

            m_forwardingStrategy->OnInterest (face, header, p/*original packet*/);
            // if (header->GetNack () > 0)
            //   OnNack (face, header, p/*original packet*/);
            // else
            //   OnInterest (face, header, p/*original packet*/);  
            break;
          }
        case NdnHeaderHelper::CONTENT_OBJECT:
          {
            Ptr<NdnContentObjectHeader> header = Create<NdnContentObjectHeader> ();
            
            static NdnContentObjectTail contentObjectTrailer; //there is no data in this object

            // Deserialization. Exception may be thrown
            packet->RemoveHeader (*header);
            packet->RemoveTrailer (contentObjectTrailer);

            m_forwardingStrategy->OnData (face, header, packet/*payload*/, p/*original packet*/);  
            break;
          }
        }
      
      // exception will be thrown if packet is not recognized
    }
  catch (NdnUnknownHeaderException)
    {
      NS_ASSERT_MSG (false, "Unknown Ndn header. Should not happen");
      NS_LOG_ERROR ("Unknown Ndn header. Should not happen");
      return;
    }
}

// void
// NdnL3Protocol::OnNack (const Ptr<NdnFace> &incomingFace,
//                         Ptr<NdnInterestHeader> &header,
//                         const Ptr<const Packet> &packet)
// {
//   NS_LOG_FUNCTION (incomingFace << header << packet);
//   m_inNacks (header, incomingFace);

//   Ptr<NdnPitEntry> pitEntry = m_pit->Lookup (*header);
//   if (pitEntry == 0)
//     {
//       // somebody is doing something bad
//       m_dropNacks (header, NON_DUPLICATED, incomingFace);
//       return;
//     }
  
//   // NdnPitEntryIncomingFaceContainer::type::iterator inFace = pitEntry->GetIncoming ().find (incomingFace);
//   NdnPitEntryOutgoingFaceContainer::type::iterator outFace = pitEntry->GetOutgoing ().find (incomingFace);

//   if (outFace == pitEntry->GetOutgoing ().end ())
//     {
// //      NS_ASSERT_MSG (false,
// //                     "Node " << GetObject<Node> ()->GetId () << ", outgoing entry should exist for face " << boost::cref(*incomingFace) << "\n" <<
// //                     "size: " << pitEntry.GetOutgoing ().size ());
      
//       // m_dropNacks (header, NON_DUPLICATE, incomingFace);
//       return;
//     }

//   // This was done in error. Never, never do anything, except normal leakage. This way we ensure that we will not have losses,
//   // at least when there is only one client
//   //
//   // incomingFace->LeakBucketByOnePacket ();

//   NS_LOG_ERROR ("Nack on " << boost::cref(*incomingFace));
  
//   pitEntry->SetWaitingInVain (outFace);

//   // If NACK is NACK_GIVEUP_PIT, then neighbor gave up trying to and removed it's PIT entry.
//   // So, if we had an incoming entry to this neighbor, then we can remove it now

//   if (header->GetNack () == NdnInterestHeader::NACK_GIVEUP_PIT)
//     {
//       pitEntry->RemoveIncoming (incomingFace);
//     }

//   pitEntry->GetFibEntry ()->UpdateStatus (incomingFace, NdnFibFaceMetric::NDN_FIB_YELLOW);
//   // StaticCast<NdnFibImpl> (m_fib)->modify (pitEntry->GetFibEntry (),
//   //                                           ll::bind (&NdnFibEntry::UpdateStatus,
//   //                                                     ll::_1, incomingFace, NdnFibFaceMetric::NDN_FIB_YELLOW));

//   if (pitEntry->GetIncoming ().size () == 0) // interest was actually satisfied
//     {
//       // no need to do anything
//       m_dropNacks (header, AFTER_SATISFIED, incomingFace);
//       return;
//     }

//   if (!pitEntry->AreAllOutgoingInVain ()) // not all ougtoing are in vain
//     {
//       NS_LOG_DEBUG ("Not all outgoing are in vain");
//       // suppress
//       // Don't do anything, we are still expecting data from some other face
//       m_dropNacks (header, SUPPRESSED, incomingFace);
//       return;
//     }
  
//   Ptr<Packet> nonNackInterest = Create<Packet> ();
//   header->SetNack (NdnInterestHeader::NORMAL_INTEREST);
//   nonNackInterest->AddHeader (*header);
  
//   bool propagated = m_forwardingStrategy->
//     PropagateInterest (pitEntry, incomingFace, header, nonNackInterest);

//   // // ForwardingStrategy will try its best to forward packet to at least one interface.
//   // // If no interests was propagated, then there is not other option for forwarding or
//   // // ForwardingStrategy failed to find it. 
//   if (!propagated)
//     {
//       m_dropNacks (header, NO_FACES, incomingFace); // this headers doesn't have NACK flag set
//       GiveUpInterest (pitEntry, header);
//     }
// }

// Processing Interests
//
// // !!! Key point.
// // !!! All interests should be answerred!!! Either later with data, immediately with data, or immediately with NACK
// void NdnL3Protocol::OnInterest (const Ptr<NdnFace> &incomingFace,
//                                  Ptr<NdnInterestHeader> &header,
//                                  const Ptr<const Packet> &packet)
// {
//   m_inInterests (header, incomingFace);

//   Ptr<NdnPitEntry> pitEntry = m_pit->Lookup (*header);
//   if (pitEntry == 0)
//     {
//       pitEntry = m_pit->Create (header);
//     }

//   if (pitEntry == 0)
//     {
//       // if it is still not created, then give up processing
//       m_dropInterests (header, PIT_LIMIT, incomingFace);
//       return;
//     }
  
//   bool isNew = pitEntry->GetIncoming ().size () == 0 && pitEntry->GetOutgoing ().size () == 0;
//   bool isDuplicated = true;
//   if (!pitEntry->IsNonceSeen (header->GetNonce ()))
//     {
//       pitEntry->AddSeenNonce (header->GetNonce ());
//       isDuplicated = false;
//     }

//   NS_LOG_FUNCTION (header->GetName () << header->GetNonce () << boost::cref (*incomingFace) << isDuplicated);

//   /////////////////////////////////////////////////////////////////////////////////////////
//   /////////////////////////////////////////////////////////////////////////////////////////
//   /////////////////////////////////////////////////////////////////////////////////////////
//   //                                                                                     //
//   // !!!! IMPORTANT CHANGE !!!! Duplicate interests will create incoming face entry !!!! //
//   //                                                                                     //
//   /////////////////////////////////////////////////////////////////////////////////////////
//   /////////////////////////////////////////////////////////////////////////////////////////
//   /////////////////////////////////////////////////////////////////////////////////////////
  
//   // Data is not in cache
//   NdnPitEntry::in_iterator inFace   = pitEntry->GetIncoming ().find (incomingFace);
//   NdnPitEntry::out_iterator outFace = pitEntry->GetOutgoing ().find (incomingFace);

//   bool isRetransmitted = false;
  
//   if (inFace != pitEntry->GetIncoming ().end ())
//     {
//       // NdnPitEntryIncomingFace.m_arrivalTime keeps track arrival time of the first packet... why?

//       isRetransmitted = true;
//       // this is almost definitely a retransmission. But should we trust the user on that?
//     }
//   else
//     {
//       inFace = pitEntry->AddIncoming (incomingFace);
//     }
//   //////////////////////////////////////////////////////////////////////////////////
//   //////////////////////////////////////////////////////////////////////////////////
//   //////////////////////////////////////////////////////////////////////////////////
  
//   if (isDuplicated) 
//     {
//       NS_LOG_DEBUG ("Received duplicatie interest on " << *incomingFace);
//       m_dropInterests (header, DUPLICATED, incomingFace);

//       /**
//        * This condition will handle "routing" loops and also recently satisfied interests.
//        * Every time interest is satisfied, PIT entry (with empty incoming and outgoing faces)
//        * is kept for another small chunk of time.
//        */

//       if (m_nacksEnabled)
//         {
//           NS_LOG_DEBUG ("Sending NACK_LOOP");
//           header->SetNack (NdnInterestHeader::NACK_LOOP);
//           Ptr<Packet> nack = Create<Packet> ();
//           nack->AddHeader (*header);

//           incomingFace->Send (nack);
//           m_outNacks (header, incomingFace);
//         }
      
//       return;
//     }

//   Ptr<Packet> contentObject;
//   Ptr<const NdnContentObjectHeader> contentObjectHeader; // used for tracing
//   Ptr<const Packet> payload; // used for tracing
//   tie (contentObject, contentObjectHeader, payload) = m_contentStore->Lookup (header);
//   if (contentObject != 0)
//     {
//       NS_ASSERT (contentObjectHeader != 0);      
//       NS_LOG_LOGIC("Found in cache");

//       OnDataDelayed (contentObjectHeader, payload, contentObject);
//       return;
//     }

//   // update PIT entry lifetime
//   pitEntry->UpdateLifetime (header->GetInterestLifetime ());
  
//   if (outFace != pitEntry->GetOutgoing ().end ())
//     {
//       NS_LOG_DEBUG ("Non duplicate interests from the face we have sent interest to. Don't suppress");
//       // got a non-duplicate interest from the face we have sent interest to
//       // Probably, there is no point in waiting data from that face... Not sure yet

//       // If we're expecting data from the interface we got the interest from ("producer" asks us for "his own" data)
//       // Mark interface YELLOW, but keep a small hope that data will come eventually.

//       // ?? not sure if we need to do that ?? ...
      
//       pitEntry->GetFibEntry ()->UpdateStatus (incomingFace, NdnFibFaceMetric::NDN_FIB_YELLOW);
//       // StaticCast<NdnFibImpl> (m_fib)->modify(pitEntry->GetFibEntry (),
//       //                                          ll::bind (&NdnFibEntry::UpdateStatus,
//       //                                                    ll::_1, incomingFace, NdnFibFaceMetric::NDN_FIB_YELLOW));
//     }
//   else
//     if (!isNew && !isRetransmitted)
//       {
//         // Suppress this interest if we're still expecting data from some other face
//         NS_LOG_DEBUG ("Suppress interests");
//         m_dropInterests (header, SUPPRESSED, incomingFace);
//         return;
//       }
  
//   /////////////////////////////////////////////////////////////////////
//   // Propagate
//   /////////////////////////////////////////////////////////////////////
  
//   bool propagated = m_forwardingStrategy->
//     PropagateInterest (pitEntry, incomingFace, header, packet);

//   if (!propagated && isRetransmitted) //give another chance if retransmitted
//     {
//       // increase max number of allowed retransmissions
//       pitEntry->IncreaseAllowedRetxCount ();

//       // try again
//       propagated = m_forwardingStrategy->
//         PropagateInterest (pitEntry, incomingFace, header, packet);
//     }
  
//   // ForwardingStrategy will try its best to forward packet to at least one interface.
//   // If no interests was propagated, then there is not other option for forwarding or
//   // ForwardingStrategy failed to find it. 
//   if (!propagated)
//     {
//       NS_LOG_DEBUG ("Not propagated");
//       m_dropInterests (header, NO_FACES, incomingFace);
//       GiveUpInterest (pitEntry, header);
//     }
// }

// void
// NdnL3Protocol::OnDataDelayed (Ptr<const NdnContentObjectHeader> header,
//                                Ptr<const Packet> payload,
//                                const Ptr<const Packet> &packet)
// {
//   // 1. Lookup PIT entry
//   Ptr<NdnPitEntry> pitEntry = m_pit->Lookup (*header);
//   if (pitEntry != 0)
//     {
//       //satisfy all pending incoming Interests
//       BOOST_FOREACH (const NdnPitEntryIncomingFace &incoming, pitEntry->GetIncoming ())
//         {
//           incoming.m_face->Send (packet->Copy ());
//           m_outData (header, payload, false, incoming.m_face);
//           NS_LOG_DEBUG ("Satisfy " << *incoming.m_face);
          
//           // successfull forwarded data trace
//         }

//       if (pitEntry->GetIncoming ().size () > 0)
//         {
//           // All incoming interests are satisfied. Remove them
//           pitEntry->ClearIncoming ();

//           // Remove all outgoing faces
//           pitEntry->ClearOutgoing ();
          
//           // Set pruning timout on PIT entry (instead of deleting the record)
//           m_pit->MarkErased (pitEntry);
//         }
//     }
//   else
//     {
//       NS_LOG_DEBUG ("Pit entry not found (was satisfied and removed before)");
//       return; // do not process unsoliced data packets
//     }
// }

// // Processing ContentObjects
// void
// NdnL3Protocol::OnData (const Ptr<NdnFace> &incomingFace,
//                         Ptr<NdnContentObjectHeader> &header,
//                         Ptr<Packet> &payload,
//                         const Ptr<const Packet> &packet)
// {
    
//   NS_LOG_FUNCTION (incomingFace << header->GetName () << payload << packet);
//   m_inData (header, payload, incomingFace);
  
//   // 1. Lookup PIT entry
//   Ptr<NdnPitEntry> pitEntry = m_pit->Lookup (*header);
//   if (pitEntry != 0)
//     {
//       // Note that with MultiIndex we need to modify entries indirectly

//       NdnPitEntry::out_iterator out = pitEntry->GetOutgoing ().find (incomingFace);
  
//       // If we have sent interest for this data via this face, then update stats.
//       if (out != pitEntry->GetOutgoing ().end ())
//         {
//           pitEntry->GetFibEntry ()->UpdateFaceRtt (incomingFace, Simulator::Now () - out->m_sendTime);
//           // StaticCast<NdnFibImpl> (m_fib)->modify (pitEntry->GetFibEntry (),
//           //                                          ll::bind (&NdnFibEntry::UpdateFaceRtt,
//           //                                                    ll::_1,
//           //                                                    incomingFace,
//           //                                                    Simulator::Now () - out->m_sendTime));
//         }
//       else
//         {
//           // Unsolicited data, but we're interested in it... should we get it?
//           // Potential hole for attacks

//           if (m_cacheUnsolicitedData)
//             {
//               // Optimistically add or update entry in the content store
//               m_contentStore->Add (header, payload);
//             }
//           else
//             {
//               NS_LOG_ERROR ("Node "<< m_node->GetId() <<
//                             ". PIT entry for "<< header->GetName ()<<" is valid, "
//                             "but outgoing entry for interface "<< boost::cref(*incomingFace) <<" doesn't exist\n");
//             }
//           // ignore unsolicited data
//           return;
//         }

//       // Update metric status for the incoming interface in the corresponding FIB entry
//       pitEntry->GetFibEntry ()->UpdateStatus (incomingFace, NdnFibFaceMetric::NDN_FIB_GREEN);
//       // StaticCast<NdnFibImpl>(m_fib)->modify (pitEntry->GetFibEntry (),
//       //                                          ll::bind (&NdnFibEntry::UpdateStatus, ll::_1,
//       //                                                    incomingFace, NdnFibFaceMetric::NDN_FIB_GREEN));
  
//       // Add or update entry in the content store
//       m_contentStore->Add (header, payload);

//       pitEntry->RemoveIncoming (incomingFace);

//       if (pitEntry->GetIncoming ().size () == 0)
//         {
//           // Set pruning timout on PIT entry (instead of deleting the record)
//           m_pit->MarkErased (pitEntry);
//         }
//       else
//         {
//           OnDataDelayed (header, payload, packet);
//         }
//     }
//   else
//     {
//       NS_LOG_DEBUG ("Pit entry not found");
//       if (m_cacheUnsolicitedData)
//         {
//           // Optimistically add or update entry in the content store
//           m_contentStore->Add (header, payload);
//         }
//       else
//         {
//           // Drop data packet if PIT entry is not found
//           // (unsolicited data packets should not "poison" content store)
      
//           //drop dulicated or not requested data packet
//           m_dropData (header, payload, UNSOLICITED, incomingFace);
//         }
//       return; // do not process unsoliced data packets
//     }
// }

// void
// NdnL3Protocol::GiveUpInterest (Ptr<NdnPitEntry> pitEntry,
//                                 Ptr<NdnInterestHeader> header)
// {
//   NS_LOG_FUNCTION (this);

//   if (m_nacksEnabled)
//     {
//       Ptr<Packet> packet = Create<Packet> ();
//       header->SetNack (NdnInterestHeader::NACK_GIVEUP_PIT);
//       packet->AddHeader (*header);

//       BOOST_FOREACH (const NdnPitEntryIncomingFace &incoming, pitEntry->GetIncoming ())
//         {
//           NS_LOG_DEBUG ("Send NACK for " << boost::cref (header->GetName ()) << " to " << boost::cref (*incoming.m_face));
//           incoming.m_face->Send (packet->Copy ());

//           m_outNacks (header, incoming.m_face);
//         }
  
//       // All incoming interests cannot be satisfied. Remove them
//       pitEntry->ClearIncoming ();

//       // Remove also outgoing
//       pitEntry->ClearOutgoing ();
  
//       // Set pruning timout on PIT entry (instead of deleting the record)
//       m_pit->MarkErased (pitEntry);
//     }
// }

} //namespace ns3