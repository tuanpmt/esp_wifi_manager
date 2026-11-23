import { useState } from 'preact/hooks';
import { Card } from './ui/Card';
import { Button } from './ui/Button';
import type { ScanResult } from '../types';
import { api } from '../api/client';
import './NetworkList.css';

interface Props {
  onConnect: (ssid: string) => void;
}

export function NetworkList({ onConnect }: Props) {
  const [networks, setNetworks] = useState<ScanResult[]>([]);
  const [scanning, setScanning] = useState(false);
  const [selectedSsid, setSelectedSsid] = useState<string | null>(null);
  const [password, setPassword] = useState('');
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [showHidden, setShowHidden] = useState(false);
  const [hiddenSsid, setHiddenSsid] = useState('');

  const scan = async () => {
    setScanning(true);
    try {
      const results = await api.scan();
      setNetworks(results);
    } catch (e) {
      console.error('Scan failed:', e);
    } finally {
      setScanning(false);
    }
  };

  const connect = async (ssid: string, open: boolean) => {
    setConnecting(true);
    setError(null);
    try {
      // Save the network when credentials are given, or when it is open
      // (an unsaved open network cannot be connected to by SSID alone).
      // Leaving the password empty for a secured network reuses the saved one.
      if (password || open) {
        await api.addNetwork(ssid, password);
      }
      await api.connect(ssid);
      onConnect(ssid);
      setSelectedSsid(null);
      setShowHidden(false);
      setHiddenSsid('');
      setPassword('');
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Connect failed');
    } finally {
      setConnecting(false);
    }
  };

  // SSIDs are at most 32 bytes (UTF-8), not 32 characters
  const ssidTooLong = new TextEncoder().encode(hiddenSsid).length > 32;

  return (
    <Card
      title="Available Networks"
      action={
        <Button size="sm" variant="secondary" onClick={scan} loading={scanning}>
          Scan
        </Button>
      }
    >
      {error && <div class="network-error text-sm">{error}</div>}

      {networks.length === 0 ? (
        <div class="network-empty">
          {scanning ? 'Scanning...' : 'Click Scan to find networks'}
        </div>
      ) : (
        <div class="network-list">
          {networks.map((net) => (
            <div class="network-item" key={net.ssid}>
              <div class="network-info">
                <div class="network-ssid">
                  {net.auth !== 'OPEN' && <span class="lock">🔒</span>}
                  {net.ssid}
                </div>
                <div class="network-meta text-sm text-muted">
                  {net.rssi} dBm • {net.auth}
                </div>
              </div>
              <Button
                size="sm"
                onClick={() => setSelectedSsid(selectedSsid === net.ssid ? null : net.ssid)}
              >
                {selectedSsid === net.ssid ? 'Cancel' : 'Connect'}
              </Button>

              {selectedSsid === net.ssid && (
                <div class="network-connect-form">
                  {net.auth !== 'OPEN' && (
                    <input
                      type="password"
                      placeholder="Password"
                      value={password}
                      onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
                    />
                  )}
                  <Button
                    onClick={() => connect(net.ssid, net.auth === 'OPEN')}
                    loading={connecting}
                    class="w-full"
                  >
                    Connect
                  </Button>
                </div>
              )}
            </div>
          ))}
        </div>
      )}

      <div class="network-hidden">
        <Button
          size="sm"
          variant="secondary"
          onClick={() => {
            setShowHidden(!showHidden);
            setSelectedSsid(null);
            setPassword('');
          }}
        >
          {showHidden ? 'Cancel' : 'Hidden network...'}
        </Button>

        {showHidden && (
          <div class="network-connect-form">
            <input
              type="text"
              placeholder="Network name (SSID)"
              value={hiddenSsid}
              maxLength={32}
              onInput={(e) => setHiddenSsid((e.target as HTMLInputElement).value)}
            />
            <input
              type="password"
              placeholder="Password (leave empty for open network)"
              value={password}
              onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
            />
            {ssidTooLong && <div class="network-error text-sm">SSID must be at most 32 bytes</div>}
            <Button
              onClick={() => connect(hiddenSsid, password === '')}
              loading={connecting}
              disabled={!hiddenSsid || ssidTooLong}
              class="w-full"
            >
              Connect
            </Button>
          </div>
        )}
      </div>
    </Card>
  );
}
