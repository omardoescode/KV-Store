import http from 'k6/http';

const BASE_URL = __ENV.BASE_URL || 'http://127.0.0.1:7890';
const READ_PCT = Number(__ENV.READ_PCT || 95);
const KEYS = Number(__ENV.KEYS || 10000);
const VALUE_SIZE = Number(__ENV.VALUE_SIZE || 100);
const ZIPF_S = Number(__ENV.ZIPF_S || 0.99);
const P99_MS = Number(__ENV.P99_MS || 50);
const MODE = __ENV.MODE || 'ramp';
const PRELOAD = (__ENV.PRELOAD || '1') !== '0';

// step: one constant-arrival-rate run at RATE. ramp: START_RATE to MAX_RATE in STAGES steps,
// each ramped over RAMP and held for HOLD, aborted by the thresholds at saturation.
const RATE = Number(__ENV.RATE || 1000);
const DURATION = __ENV.DURATION || '20s';
const START_RATE = Number(__ENV.START_RATE || 1000);
const MAX_RATE = Number(__ENV.MAX_RATE || 100000);
const STAGES = Number(__ENV.STAGES || 10);
const RAMP = __ENV.RAMP || '5s';
const HOLD = __ENV.HOLD || '15s';

const peak = MODE === 'step' ? RATE : MAX_RATE;
// VUs that k6 must start mid-run make it drop iterations, so preallocate for stalls of ~20 ms.
const PRE_VUS = Number(__ENV.PRE_VUS || Math.max(50, Math.ceil(peak / 50)));
const MAX_VUS = Math.max(PRE_VUS, Number(__ENV.MAX_VUS || Math.max(200, Math.ceil(peak / 20))));

const stages = [];
for (let i = 1; i <= STAGES; i++) {
  const target = Math.round(START_RATE * Math.pow(MAX_RATE / START_RATE, i / STAGES));
  stages.push({ target, duration: RAMP }, { target, duration: HOLD });
}

const scenario =
  MODE === 'step'
    ? { executor: 'constant-arrival-rate', rate: RATE, duration: DURATION }
    : { executor: 'ramping-arrival-rate', startRate: START_RATE, stages };

const abort = (threshold) => ({ threshold, abortOnFail: true, delayAbortEval: '5s' });

export const options = {
  discardResponseBodies: true,
  summaryTrendStats: ['avg', 'min', 'med', 'p(95)', 'p(99)', 'max'],
  setupTimeout: '120s',
  scenarios: {
    main: { ...scenario, timeUnit: '1s', preAllocatedVUs: PRE_VUS, maxVUs: MAX_VUS },
  },
  thresholds: {
    'http_req_failed{phase:main}': [abort('rate<0.01')],
    'http_req_duration{phase:main}': [abort(`p(99)<${P99_MS}`)],
    'http_req_duration{op:read}': [`p(99)<${P99_MS}`],
    'http_req_duration{op:write}': [`p(99)<${P99_MS}`],
    'http_req_failed{op:read}': ['rate<0.01'],
    'http_req_failed{op:write}': ['rate<0.01'],
  },
};

// Rank r is drawn with weight 1/r^ZIPF_S, so key_0 is the hottest.
const cdf = new Float64Array(KEYS);
let total = 0;
for (let r = 0; r < KEYS; r++) {
  total += 1 / Math.pow(r + 1, ZIPF_S);
  cdf[r] = total;
}
for (let r = 0; r < KEYS; r++) {
  cdf[r] /= total;
}

function zipfKey() {
  const u = Math.random();
  let lo = 0;
  let hi = KEYS - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (cdf[mid] < u) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return `key_${lo}`;
}

const body = JSON.stringify({ value: 'x'.repeat(VALUE_SIZE) });
const putParams = (tags) => ({ headers: { 'Content-Type': 'application/json' }, tags });

export function setup() {
  if (!PRELOAD) {
    return;
  }
  const params = putParams({ op: 'preload', name: 'kv' });
  for (let i = 0; i < KEYS; i += 200) {
    const batch = [];
    for (let k = i; k < Math.min(i + 200, KEYS); k++) {
      batch.push(['PUT', `${BASE_URL}/kv/key_${k}`, body, params]);
    }
    for (const res of http.batch(batch)) {
      if (res.status !== 200) {
        throw new Error(`preload PUT ${res.url}: status ${res.status}`);
      }
    }
  }
}

const readParams = { tags: { op: 'read', phase: 'main', name: 'kv' } };
const writeParams = putParams({ op: 'write', phase: 'main', name: 'kv' });

export default function () {
  const url = `${BASE_URL}/kv/${zipfKey()}`;
  if (Math.random() * 100 < READ_PCT) {
    http.get(url, readParams);
  } else {
    http.put(url, body, writeParams);
  }
}

export function handleSummary(data) {
  const out = { stdout: `k6 summary: ${__ENV.SUMMARY_PATH || '(no SUMMARY_PATH)'}\n` };
  if (__ENV.SUMMARY_PATH) {
    out[__ENV.SUMMARY_PATH] = JSON.stringify(data, null, 2);
  }
  return out;
}
