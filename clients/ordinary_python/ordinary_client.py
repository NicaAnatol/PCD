#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import socket
import sys
import os
import struct
import math
import time
import threading

OPR_UPLOAD_GEO = 1
OPR_BYE = 6
OPR_LOGIN = 10
OPR_REGISTER = 11
OPR_CHECK_TASK = 20
OPR_GET_RESULT = 21
OPR_CANCEL_TASK = 32
OPR_UPLOAD_FILE = 50
OPR_DOWNLOAD_FILE = 51

class GeoClient:
    def __init__(self, host='127.0.0.1', port=18081):
        self.host = host
        self.port = port
        self.sock = None
        self.session_id = None
        self.current_user = None
        self.debug = False
        self.pending_tasks = {}
        self.next_request_id = 1
        self.request_lock = threading.Lock()
        self.running = True

    def debug_print(self, *args):
        if self.debug:
            print("[DEBUG]", *args)

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self.sock.connect((self.host, self.port))
            self.debug_print(f"Conectat la {self.host}:{self.port}")
            return True
        except Exception as e:
            print(f"Eroare conectare: {e}")
            return False

    def recv_exact(self, size):
        data = b''
        while len(data) < size:
            chunk = self.sock.recv(size - len(data))
            if not chunk:
                raise ConnectionError("Conexiune inchisa de server")
            data += chunk
        return data

    def get_next_request_id(self):
        with self.request_lock:
            req_id = self.next_request_id
            self.next_request_id += 1
            return req_id

    def write_single_int(self, client_id, op_id, value, request_id):
        msg_size = 16
        header = struct.pack('!IIII', msg_size, client_id, op_id, request_id)
        payload = struct.pack('!I', value)
        self.sock.send(header)
        self.sock.send(payload)

    def read_single_int(self):
        data = self.recv_exact(20)
        request_id = struct.unpack('!I', data[12:16])[0]
        value = struct.unpack('!I', data[16:20])[0]
        return value, request_id

    def write_single_string(self, client_id, op_id, string, request_id):
        b = string.encode('utf-8')
        str_size = len(b)
        msg_size = 16 + 4 + str_size
        header = struct.pack('!IIII', msg_size, client_id, op_id, request_id)
        len_payload = struct.pack('!I', str_size)
        self.sock.send(header)
        self.sock.send(len_payload)
        self.sock.send(b)

    def read_single_string(self):
        header = self.recv_exact(16)
        request_id = struct.unpack('!I', header[12:16])[0]
        len_data = self.recv_exact(4)
        str_len = struct.unpack('!I', len_data)[0]
        str_data = self.recv_exact(str_len)
        return str_data.decode('utf-8'), request_id
    
    def do_login(self):
        try:
            user = input("Utilizator: ").strip()
            password = input("Parola: ").strip()
        except KeyboardInterrupt:
            print("\nOperatie anulata.")
            return False
        
        req_id = self.get_next_request_id()
        self.write_single_string(0, OPR_LOGIN, user, req_id)
        self.write_single_string(0, OPR_LOGIN, password, req_id)
        
        session_id, resp_req_id = self.read_single_int()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        
        if session_id > 0:
            self.session_id = session_id
            self.current_user = user
            print(f"Autentificare reusita! Session ID: {self.session_id}")
            return True
        else:
            print("Autentificare esuata!")
            return False
    
    def do_register(self):
        try:
            user = input("Nou utilizator: ").strip()
            password = input("Parola noua: ").strip()
        except KeyboardInterrupt:
            print("\nOperatie anulata.")
            return False
        
        req_id = self.get_next_request_id()
        self.write_single_string(0, OPR_REGISTER, user, req_id)
        self.write_single_string(0, OPR_REGISTER, password, req_id)
        
        session_id, resp_req_id = self.read_single_int()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        
        if session_id > 0:
            self.session_id = session_id
            self.current_user = user
            print(f"Cont creat cu succes! Session ID: {self.session_id}")
            return True
        print("Eroare la creare cont!")
        return False
    
    def check_task_status(self, task_id):
        """Verifică statusul unui task"""
        req_id = self.get_next_request_id()
        print(f"[DEBUG] Sending CHECK_TASK for task_id={task_id}, session_id={self.session_id}, request_id={req_id}")
        self.write_single_int(self.session_id, OPR_CHECK_TASK, task_id, req_id)
        print("[DEBUG] Waiting for response...")
        status, resp_req_id = self.read_single_string()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        print(f"[DEBUG] Received status: '{status}'")
        print(f"{status}")
    
    def get_task_result(self, task_id):
        """Obține rezultatele unui task finalizat"""
        req_id = self.get_next_request_id()
        self.write_single_int(self.session_id, OPR_GET_RESULT, task_id, req_id)
        
        try:
            total_distance_str, resp_req_id = self.read_single_string()
            if resp_req_id != req_id:
                print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
            
            point_count_str, _ = self.read_single_string()
            segment_count_str, _ = self.read_single_string()
            
            if total_distance_str.startswith("ERROR:"):
                print(f"{total_distance_str}")
                return
            
            total_distance = float(total_distance_str)
            point_count = int(point_count_str)
            segment_count = int(segment_count_str)
            
            segment_distances = []
            for i in range(segment_count):
                dist_str, _ = self.read_single_string()
                segment_distances.append(float(dist_str))
            
            direct_distance_str, _ = self.read_single_string()
            route_distance_str, _ = self.read_single_string()
            has_req_str, _ = self.read_single_string()
            show_seg_resp_str, _ = self.read_single_string()
            
            direct_distance = float(direct_distance_str) if direct_distance_str else 0.0
            route_distance = float(route_distance_str) if route_distance_str else 0.0
            has_distance_request = int(has_req_str) if has_req_str else 0
            show_segments_resp = int(show_seg_resp_str) if show_seg_resp_str else 0
            
            print(f"\n=== REZULTATE DE LA SERVER (Task {task_id}) ===")
            print(f"Puncte: {point_count}")
            print(f"Distanta totala: {total_distance:.2f} km")
            
            if show_segments_resp and segment_count > 0:
                print("\n=== DISTANTE PE SEGMENTE ===")
                total = 0
                for i, dist in enumerate(segment_distances):
                    total += dist
                    pct = (dist / total) * 100 if total > 0 else 0
                    print(f"Segment {i+1}-{i+2}: {dist:.2f} km ({pct:.1f}%)")
            
            if has_distance_request:
                print(f"\n=== DISTANTA INTRE PUNCTE ===")
                print(f"Distanta directa: {direct_distance:.2f} km")
                print(f"Distanta pe traseu: {route_distance:.2f} km")
                
        except (ConnectionError, socket.error) as e:
            print(f"Eroare la primirea rezultatelor: {e}")
    
    def cancel_task(self, task_id):
        """Anulează un task (dacă nu a fost deja procesat)"""
        req_id = self.get_next_request_id()
        self.write_single_int(self.session_id, OPR_CANCEL_TASK, task_id, req_id)
        response, resp_req_id = self.read_single_string()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        print(f"{response}")
    

    def download_task_result(self, task_id):
        """Descarcă fișierul rezultat al unui task finalizat"""
        req_id = self.get_next_request_id()
        self.write_single_int(self.session_id, OPR_DOWNLOAD_FILE, task_id, req_id)
        
        # Primește numele fișierului
        filename, resp_req_id = self.read_single_string()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        
        # Primește dimensiunea
        size, resp_req_id = self.read_single_int()
        
        # Creează directorul download/ dacă nu există
        os.makedirs("download", exist_ok=True)
        
        output_path = os.path.join("download", filename)
        
        print(f"[CLIENT] Downloading file: {filename}, size: {size} bytes")
        
        # Primește conținutul fișierului
        received = 0
        with open(output_path, 'wb') as f:
            while received < size:
                chunk_size = min(8192, size - received)
                chunk = self.sock.recv(chunk_size)
                if not chunk:
                    raise ConnectionError("Conexiune inchisa in timpul descarcarii")
                f.write(chunk)
                received += len(chunk)
        
        print(f"\n=== DOWNLOAD COMPLET ===")
        print(f"Fisier salvat: {output_path} ({received} bytes)")
    
    def read_points_from_file(self, filename):
        points = []
        try:
            with open(filename, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith('#'):
                        parts = line.split(',')
                        if len(parts) >= 2:
                            try:
                                lat = float(parts[0].strip())
                                lon = float(parts[1].strip())
                                points.append((lat, lon))
                            except ValueError:
                                pass
        except Exception as e:
            print(f"Eroare citire fisier: {e}")
        return points

    def send_points_to_server(self, points, filename="manual_input", bbox=None, epsilon=-1, 
                               show_segments=0, dist_idx1=0, dist_idx2=0):
        if not points:
            print("Eroare: niciun punct de uploadat")
            return False

        print(f"\nUpload a {len(points)} puncte...")
        
        req_id = self.get_next_request_id()
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, filename, req_id)
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(len(points)), req_id)
        
        bbox_str = bbox if bbox else ""
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, bbox_str, req_id)
        
        epsilon_str = str(epsilon) if epsilon > 0 else ""
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, epsilon_str, req_id)
        
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(show_segments), req_id)
        
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(dist_idx1), req_id)
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(dist_idx2), req_id)
        
        for lat, lon in points:
            coord_str = f"{lat},{lon}"
            self.write_single_string(self.session_id, OPR_UPLOAD_GEO, coord_str, req_id)
        
        # Primește task_id în loc de rezultate
        task_id, resp_req_id = self.read_single_int()
        if resp_req_id != req_id:
            print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
        
        print(f"\n=== UPLOAD INITIAT ===")
        print(f"Task ID: {task_id}")
        print("Folositi 'status <id>' pentru a verifica progresul")
        print("Folositi 'result <id>' pentru a obtine rezultatele cand task-ul este finalizat")
        
        self.pending_tasks[task_id] = filename
        return True

    def upload_raw_file(self, filename, bbox=None, epsilon=-1, show_segments=0, dist_idx1=0, dist_idx2=0):
        """Upload fișier brut (fără parsare în client) - chunked transfer"""
        try:
            # Deschide fișierul
            with open(filename, 'rb') as f:
                f.seek(0, os.SEEK_END)
                file_size = f.tell()
                f.seek(0, os.SEEK_SET)
                
                print(f"[CLIENT] File: {filename}, size: {file_size} bytes")
                
                req_id = self.get_next_request_id()
                
                # Trimite numele fișierului
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, os.path.basename(filename), req_id)
                
                # Trimite dimensiunea
                self.write_single_int(self.session_id, OPR_UPLOAD_FILE, file_size, req_id)
                
                # Trimite parametrii GEO
                bbox_str = bbox if bbox else ""
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, bbox_str, req_id)
                
                epsilon_str = str(epsilon) if epsilon > 0 else ""
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, epsilon_str, req_id)
                
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, str(show_segments), req_id)
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, str(dist_idx1), req_id)
                self.write_single_string(self.session_id, OPR_UPLOAD_FILE, str(dist_idx2), req_id)
                
                print("[CLIENT] GEO params sent, starting file transfer...")
                
                # Trimite conținutul fișierului în chunk-uri brute
                chunk_size = 8192
                total_sent = 0
                chunk_num = 0
                
                while True:
                    chunk = f.read(chunk_size)
                    if not chunk:
                        break
                    
                    self.sock.send(chunk)
                    total_sent += len(chunk)
                    print(f"[CLIENT] Sending chunk {chunk_num}, size: {len(chunk)} bytes, total sent: {total_sent}/{file_size}")
                    chunk_num += 1
                
                print(f"[CLIENT] File transfer complete, total sent: {total_sent}/{file_size} bytes")
                
                if total_sent != file_size:
                    print("Eroare: dimensiune trimisa incorecta")
                    return -1
                
                # Primește task_id
                task_id, resp_req_id = self.read_single_int()
                if resp_req_id != req_id:
                    print(f"Warning: request_id mismatch! Sent {req_id}, got {resp_req_id}")
                
                print(f"\n=== UPLOAD FILE INITIAT ===")
                print(f"Task ID: {task_id}")
                print("Folositi 'status <id>' si 'result <id>' pentru a verifica")
                
                self.pending_tasks[task_id] = filename
                return task_id
                
        except KeyboardInterrupt:
            print("\nUpload anulat de utilizator.")
            return -1
        except Exception as e:
            print(f"Eroare la upload_raw: {e}")
            return -1

    def parse_bbox(self, bbox_str):
        parts = bbox_str.split(',')
        if len(parts) == 4:
            return f"{parts[0]},{parts[1]},{parts[2]},{parts[3]}"
        return None

    def parse_points_from_args(self, args):
        points = []
        for arg in args:
            parts = arg.split(',')
            if len(parts) >= 2:
                try:
                    lat = float(parts[0].strip())
                    lon = float(parts[1].strip())
                    points.append((lat, lon))
                except ValueError:
                    pass
        return points

    def close(self):
        if self.sock:
            try:
                req_id = self.get_next_request_id()
                self.write_single_int(self.session_id if self.session_id else 0, OPR_BYE, 0, req_id)
            except:
                pass
            self.sock.close()

    def print_usage(self):
        help_text = """
\033[1;33mComenzi disponibile:\033[0m
  upload <fisier>                                    - Upload simplu (parsare client)
  upload_raw <fisier>                                - Upload brut (chunked, parsare server)
  upload --bbox <min_lat,max_lat,min_lon,max_lon> <fisier>
  upload --simplify <epsilon> <fisier>               - Simplificare traseu
  upload --segments <fisier>                         - Afiseaza distante pe segmente
  upload --distance <idx1,idx2> <fisier>             - Distanta intre doua puncte
  status <task_id>                                   - Verifica statusul unui task
  result <task_id>                                   - Obtine rezultatele unui task finalizat
  cancel <task_id>                                   - Anuleaza un task (daca nu a fost procesat)
  download <task_id>                                 - Descarca fisierul rezultat al unui task finalizat
  <lat,lon> [lat,lon ...]                            - Introducere directa puncte
  help                                               - Acest mesaj
  exit                                               - Iesire

\033[1;33mExemple:\033[0m
  upload test.csv
  upload_raw test.gpx
  upload --bbox 44,48,20,30 test.csv
  upload --simplify 0.5 test.csv
  upload --segments test.csv
  upload --distance 1,5 test.csv
  status 1
  result 1
  cancel 1
  download 1
  44.4268,26.1025 45.7489,21.2087
"""
        print(help_text)

    def run_shell(self):
        if not self.connect():
            print("Nu se poate conecta la server!")
            return

        authenticated = False
        while not authenticated and self.running:
            try:
                print("\n\033[1;36m=== CLIENT GEOSPAȚIAL (Python) ===\033[0m")
                print("1. Autentificare")
                print("2. Creare cont nou")
                print("3. Iesire")
                choice = input("Alege: ").strip()
            except KeyboardInterrupt:
                print("\nIesire requested.")
                break
            except EOFError:
                break
            
            if choice not in ['1', '2', '3']:
                print("Optiune invalida! Alegeti 1, 2 sau 3.\n")
                continue

            if choice == '1':
                if self.do_login():
                    authenticated = True
                    print(f"\nBun venit, {self.current_user}!")
                    self.print_usage()
            elif choice == '2':
                if self.do_register():
                    authenticated = True
                    print(f"\nBun venit, {self.current_user}!")
                    self.print_usage()
            elif choice == '3':
                self.close()
                return

        while self.running:
            try:
                prompt = f"\n\033[1;32m{self.current_user}@geopython\033[0m> "
                cmd = input(prompt).strip()
                if not cmd:
                    continue
                
                # Parsează comanda și argumentele
                parts = cmd.split()
                if not parts:
                    continue
                
                # NU converti la lower() - păstrează comanda originală
                command = parts[0]
                
                # Comanda de ieșire
                if command == 'exit' or command == 'quit':
                    break
                
                # Comanda help
                elif command == 'help':
                    self.print_usage()
                    continue
                
                # Comanda status
                elif command == 'status':
                    if len(parts) != 2:
                        print("Folosire: status <task_id>")
                        continue
                    try:
                        task_id = int(parts[1])
                        self.check_task_status(task_id)
                    except ValueError:
                        print("Task ID invalid. Folositi un numar.")
                    continue
                
                # Comanda result
                elif command == 'result':
                    if len(parts) != 2:
                        print("Folosire: result <task_id>")
                        continue
                    try:
                        task_id = int(parts[1])
                        self.get_task_result(task_id)
                    except ValueError:
                        print("Task ID invalid. Folositi un numar.")
                    continue
                
                # Comanda cancel
                elif command == 'cancel':
                    if len(parts) != 2:
                        print("Folosire: cancel <task_id>")
                        continue
                    try:
                        task_id = int(parts[1])
                        self.cancel_task(task_id)
                    except ValueError:
                        print("Task ID invalid. Folositi un numar.")
                    continue
                

                elif command == 'download':
                    if len(parts) != 2:
                        print("Folosire: download <task_id>")
                        continue
                    try:
                        task_id = int(parts[1])
                        self.download_task_result(task_id)
                    except ValueError:
                        print("Task ID invalid. Folositi un numar.")
                    continue
                
                # Comanda upload_raw
                elif command == 'upload_raw':
                    if len(parts) < 2:
                        print("Folosire: upload_raw <fisier> [optiuni]")
                        continue
                    
                    filename = None
                    bbox = None
                    epsilon = -1
                    show_segments = 0
                    dist_idx1 = 0
                    dist_idx2 = 0
                    
                    i = 1
                    while i < len(parts):
                        if parts[i] == '--bbox' and i + 1 < len(parts):
                            bbox = self.parse_bbox(parts[i+1])
                            i += 2
                        elif parts[i] == '--simplify' and i + 1 < len(parts):
                            epsilon = float(parts[i+1])
                            i += 2
                        elif parts[i] == '--segments':
                            show_segments = 1
                            i += 1
                        elif parts[i] == '--distance' and i + 1 < len(parts):
                            idx_parts = parts[i+1].split(',')
                            if len(idx_parts) == 2:
                                dist_idx1 = int(idx_parts[0])
                                dist_idx2 = int(idx_parts[1])
                            i += 2
                        else:
                            filename = parts[i]
                            i += 1
                    
                    if not filename:
                        print("Specificati fisierul!")
                        continue
                    
                    if not os.path.exists(filename):
                        print(f"Fisierul {filename} nu exista!")
                        continue
                    
                    self.upload_raw_file(filename, bbox, epsilon, show_segments, dist_idx1, dist_idx2)
                    continue
                
                # Comanda upload (format text)
                elif command == 'upload':
                    if len(parts) < 2:
                        print("Folosire: upload <fisier> [optiuni]")
                        continue
                    
                    filename = None
                    bbox = None
                    epsilon = -1
                    show_segments = 0
                    dist_idx1 = 0
                    dist_idx2 = 0
                    
                    i = 1
                    while i < len(parts):
                        if parts[i] == '--bbox' and i + 1 < len(parts):
                            bbox = self.parse_bbox(parts[i+1])
                            i += 2
                        elif parts[i] == '--simplify' and i + 1 < len(parts):
                            epsilon = float(parts[i+1])
                            i += 2
                        elif parts[i] == '--segments':
                            show_segments = 1
                            i += 1
                        elif parts[i] == '--distance' and i + 1 < len(parts):
                            idx_parts = parts[i+1].split(',')
                            if len(idx_parts) == 2:
                                dist_idx1 = int(idx_parts[0])
                                dist_idx2 = int(idx_parts[1])
                            i += 2
                        else:
                            filename = parts[i]
                            i += 1
                    
                    if not filename:
                        print("Specificati fisierul!")
                        continue
                    
                    if not os.path.exists(filename):
                        print(f"Fisierul {filename} nu exista!")
                        continue
                    
                    points = self.read_points_from_file(filename)
                    if not points:
                        print("Eroare: nu s-au putut citi punctele din fisier")
                        continue
                    
                    print(f"Au fost citite {len(points)} puncte din fisier")
                    self.send_points_to_server(points, os.path.basename(filename), bbox, epsilon, 
                                                show_segments, dist_idx1, dist_idx2)
                    continue
                
                # Introducere directă puncte
                else:
                    # Verifică dacă sunt puncte (format "lat,lon")
                    points = self.parse_points_from_args(parts)
                    if not points:
                        print(f"Comanda necunoscuta: '{command}'. Folositi 'help' pentru ajutor.")
                        continue
                    
                    self.send_points_to_server(points, "manual_input", None, -1, 0, 0, 0)
                        
            except KeyboardInterrupt:
                print("\nOperatie anulata. Apasati 'exit' pentru a iesi.")
                continue
            except EOFError:
                break
            except (BrokenPipeError, ConnectionError) as e:
                print(f"\nConexiune pierduta cu serverul. Va rugam restartati clientul.\n")
                break

        self.close()
        print("\nClient inchis.")

def print_instructions():
    instructions = """
\033[1;36m=== CLIENT GEOSPAȚIAL (Python) ===\033[0m
Utilizare: python3 geoclient.py [server_ip] [port]
  server_ip - adresa IP a serverului (implicit: 127.0.0.1)
  port      - portul serverului (implicit: 18081)

Exemple:
  python3 geoclient.py                    # Conectare la localhost:18081
  python3 geoclient.py 192.168.1.100      # Conectare la 192.168.1.100:18081
  python3 geoclient.py 10.0.0.1 8080      # Conectare la 10.0.0.1:8080
"""
    print(instructions)

def main():
    host = "127.0.0.1"
    port = 18081
    
    print_instructions()
    
    if len(sys.argv) > 1:
        host = sys.argv[1]
    if len(sys.argv) > 2:
        try:
            port = int(sys.argv[2])
        except ValueError:
            print("Port invalid. Folositi un numar intre 1 si 65535.")
            return
    
    print(f"\nConectare la {host}:{port}...")
    
    client = GeoClient(host, port)
    client.run_shell()

if __name__ == "__main__":
    main()