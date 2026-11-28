# Path Computation Element Service (PCES) - Server Side

This text is derived from Commsol's PCES documentation. Commsol is a server side implimentation
of PCES developed by Michael Spenard.

PCES follows the IETF's Path Computation Element architecture, RFC 4655: a service that computes
paths on request from a database of link metrics.

PCES collects TRACE packets and other data to compute link metrics and provide them to PCES clients
in the form of Best Path responses. This is desirable as MeshCore links carry no metric (i.e.,
score) on their paths. Moreover, MeshCore implements sender based routing, and repeaters carry no
routing tables. PCES, therefore, supplements MeshCore with its own metric (0–100), from SNR
measured at the receiving end of each link, per direction. Below, "the PCES station" means the
MeshCore station that hosts PCES.

Optionally, and the reason for the firmwares found here, a PCES server may collect congestion 
information from repeaters. With this further information PCES can provide clients with a Best
Path that routes around overly burdened repeaters that may have a high chance for packet collisions
etc. The details of added features this entails for a repeater can be found in link-stats.md and
load.md.

## Data collection

1. **Overheard traces:** as a trace travels, each hop appends the SNR it received the trace at
   before sending it on. A PCES station passively listens for these packets.
2. **Initiated traces:** traces the PCES station sent rather than overheard. A link counts at most
   once an hour per traced route, overheard or initiated, so frequent self-initiated traces can't
   inflate it.
3. **Beacon traces:** traces reported through observers made available by the aggregation service
   Beacon.
4. **Direct neighbors:** every packet the PCES station hears straight from a neighboring repeater,
   of course, measures that link.
5. **Neighbor tables:** repeater neighbor tables can be configured to be fetched automatically.
6. **Repeaters running this firmware:** each measures its own links from the packets it receives
   ([link stats](link-stats.md)) and its channel load ([load](load.md)), and
   answers a small request for them.

## Metric calculation, per direction

1. **Weight each measurement by age:** `w = 0.5^(age / 48 hours)`. The weighted mean `avg` and
   standard deviation `σ` follow recent readings; `w_n = Σw` is the weighted count.
2. **Margin over the decode limit:** `margin = avg − σ − limit`, with `limit = −2.5 × (SF − 4)`,
   the SNR below which LoRa can't decode at that spreading factor (SF7: −7.5 dB). PCES uses the
   spreading factor its own station's radio is set to.
3. **Quality:** `q = min(1, max(0, margin / 15 dB))`: the ratio held to 0–1, not a check that
   rejects anything. No margin or less gives 0; 15 dB of margin or more gives full marks (1).
4. **Confidence:** `c = w_n / (w_n + 3)`. Three fresh measurements give half marks, and the score
   fades while the link goes unmeasured.
5. **metric = round(100 × q × c)**

A link's **weaker direction** becomes the link's prevailing score. A Best Path is either one way or
both ways (below). A link with no measurement for 48 hours is marked stale.

## Best Path

1. The Best Path is the one whose **weakest leg scores highest**: a path is only as good as its worst
   hop.
2. If more than one path has the best score (e.g., two paths that both score 100), the one with the
   fewest hops wins. A path has at most 8 hops.

There are two kinds of Best Path:

- **One way** (a client's PATH request): each leg is scored in the direction the path goes,
  A → B, not by the link's weaker score. A direction measured only the other way stands in for the
  missing one (links aren't always symmetric). This is the better route out, but its reverse is
  unchecked. Therefore, replies will not have the benefit of PCES. As the remote end will proceed
  with its own PATH discovery.
- **Both ways** (a client's PATH BOTH WAYS request): only links measured in both directions are
  used, and each leg is scored by the link's weaker direction. The score holds whichever way the
  path is used, so its reverse is safe to send to the far end as its route back. Therefore, an
  established contact on the remote end may have the benefit of PCES. As such a symetric best path
  can be used to "seed" the remote end.


The numbers here are Commsol's default settings as of (2026-11-21).
