import matplotlib.pyplot as plt

# 1) Your measured data (replace these samples)
resolutions    = ['480p', '720p', '1080p']
throughput_tcp = [10, 20, 30]   # Mbps
throughput_udp = [12, 25, 35]   # Mbps
latency_tcp    = [50, 100, 200] # ms
latency_udp    = [40,  90, 180] # ms

# 2) Throughput vs. Resolution
plt.figure()
plt.plot(resolutions, throughput_tcp, marker='o', label='TCP')
plt.plot(resolutions, throughput_udp, marker='o', label='UDP')
plt.xlabel('Resolution')
plt.ylabel('Throughput (Mbps)')
plt.title('Throughput vs. Resolution')
plt.legend()
plt.show()

# 3) Latency vs. Resolution
plt.figure()
plt.plot(resolutions, latency_tcp, marker='o', label='TCP')
plt.plot(resolutions, latency_udp, marker='o', label='UDP')
plt.xlabel('Resolution')
plt.ylabel('Latency (ms)')
plt.title('Latency vs. Resolution')
plt.legend()
plt.show()
