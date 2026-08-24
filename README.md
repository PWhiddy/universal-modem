## Universal Modem

This tool creates a virtual network that can proxy all internet traffic on a machine through an unconventional mediums, like audio or visible light. By implementing a shared API, all mediums can be connected via a shared underlying tun/utun binding, gateway/client discovery and connection management, and 2 way data transfer to share the gateways real internet connection and give the client real functional internet access.

### Project requirements:

Usage:   

Start Gateway: `sudo ./universal-modem --audio --gateway`  

Start Client: `sudo ./universal-modem --audio --client`  

(--audio can be replaced with --light for video medium, but we are going to completely build and test audio first before starting work on light/video)

### Audio System logic:  
When the gateway is started, it listens for a client.
When the client is started, it broadcasts a connection request once every several seconds.
When the gateway hears the client request, it sends a response offering a connection.
When the client hears the offer, they confirm.
Once they have a confirmed connection, they will take turns performing a calibration step to find the best data encoding configuration. more details on this in the section below. note that this will not be the same in both directions, as each will have different mic, speaker, and location in the room. they essentially need to establish what the best possible configuration to enable maximum data bandwidth and reliability.
One the connection is calibrated, the client will start routing all of its system network requests through the gateway. The gateway will proxy all of these network requests through its own network connection. On the client, once connection is configured, they should be able to (disconnected from wifi and ethernet) open a web browser and browser, or `curl google.com` and have everything function seemlessly as normal (though

If any point of all this connection, calibration, and data transfer process the connection is intrupted, both client and server need to gracefully fall back to restablishing the connection.

### Key considerations: 

For now, we may assume there will always be one gateway and one client.

The program should be implemented in a version of C which builds with the standard toolchain bundled on modern systems.

There should be no external, non system code dependencies.

You may use make/Makefile for building, testing, ect.

The program should build and function identically on macos and on linux.

The code should be written clearly and robustly, but also concisely and not overly defensive. It should be built like a clean, high quality open source library that is readable, simple, and maintainable, while being well tested and robust.

Use 48khz audio sampling.

Data symbols should be encoded using QAM, split over multiple carriers with OFDM.

Extra robustness should be added with COFDM methods (forward error correction (convolutional coding) and time/frequency interleaving). Make sure to test this in isolation first before integrating them into a more complex system.

In the calibration process, the two systems should each sweep over different configurations to find: highly usable frequency range (min to max), QAM scheme (against each other param, try QAM-4, QAM-16, and QAM-64), cycle-prefix duration, fec/coding parameters, and any other key parameters of the system. For each of these parameters, very carefully think of a reasonable range and spacing to sweep over, but calculate how long the full sweep will take for both sides to complete, because it cannot take hours. default calibration should take very roughly 15 seconds total. there should be a --calib-high cli option which will allow up to several minutes to get the very best config, which we later can use to set a grounded real world default.

The calibration process should emit detailed logs of what its trying, and the result of each experiment.

As both client and gateway are running, each should log detailed information about their state, configuration, message decoding quality, and data transfer rates.

Received audio may be very quiet or very loud, or very noisy. system should do the best it possibly can for all of these cases.

One systems microphone can pick up its own speaker output, so the system needs to ensure that it is not listening while it is also sending anything. Additionally, this means the overall system needs to account for the fact that the if both are trying to send at the same time, neither will hear, and the system needs to robustly account for that. Or, in connection negotiation they could each reserve specific frequency bands which each communicate on in order to have simultanious send and receive without interference. that could also be useful in initial connection establishment. but splitting up bandwidth between client and gateway could half data speed so there is a tradeoff.

In the full system, carefully consider the timing of each operation from both sides, so that each part of the process can proceed as quickly as possible, but without causing them to step on each others toes. Each should model the others possible state, and can make assumptions about its timing at a high level, but leave decent wiggle room knowing that it can't predict what the other will do down to miliseconds timing because systems always have random hiccups and delays, quirks.

Everything should be built up and tested very incrementally. First test pure data encoding and decoding, then under distortion, echo, delay, noise, cut-outs, all of these combined, to make sure performance matches expectations and that failures are handled robustly.

Be aware that at the boundries of QAM encoded symbols, discontinuities can cause strong frequency artifact leakage which can hurt performance. This is something that you will want to measure, test, and decide if mitigation (like either filtering or attenuation at boundries) is needed, early in the process so that it doesn't only appear once the system has become more complicated.

After all the basic incremental testing, a complete simulated test with an async virtual client and gateway (plus distortions listed above) to fully validate connection estblishment, data transfer, connection loss and reconnection, to validate the whole system in as close to the real multi-machine setup as possible. 

To best understand real world encoding decoding & transmission distortions, we may want to record real audio from one machine to another to analze what real world distortion, noise, ect actually looks like.

Maintain (generally should append only) a log file Progress.md which tracks all key steps, tests, decisions ect.

If you have any questions that are critical, please stop and ask early before making assumptions.
