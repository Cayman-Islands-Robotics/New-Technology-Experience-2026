import { initializeApp } from 'firebase/app';
import { getFirestore } from 'firebase/firestore';

/* Web config comes from .env.local (see .env.example). */
const env = import.meta.env;

export const firebaseConfig = {
  apiKey: env.VITE_FIREBASE_API_KEY,
  authDomain: env.VITE_FIREBASE_AUTH_DOMAIN,
  projectId: env.VITE_FIREBASE_PROJECT_ID,
  storageBucket: env.VITE_FIREBASE_STORAGE_BUCKET,
  messagingSenderId: env.VITE_FIREBASE_MESSAGING_SENDER_ID,
  appId: env.VITE_FIREBASE_APP_ID,
};

/** The collection `pi_sensor_thermal_rover.py` writes to. */
export const READINGS_COLLECTION = 'readings';

/** Null when .env.local is missing, so the console can say so instead of crashing. */
export const db = firebaseConfig.apiKey && firebaseConfig.projectId
  ? getFirestore(initializeApp(firebaseConfig))
  : null;
