
#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import socket
import sys
import os
import struct
import math
import time

OPR_UPLOAD_GEO = 1
OPR_BYE = 6
OPR_LOGIN = 10
OPR_REGISTER = 11

class GeoClient:
    def __init__(self, host='127.0.0.1', port=18081):
        self.host = host
        self.port = port
        self.sock = None
        self.session_id = None
        self.current_user = None
        self.debug = False

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

    def write_single_int(self, client_id, op_id, value):
        msg_size = 16
        header = struct.pack('!III', msg_size, client_id, op_id)
        payload = struct.pack('!I', value)
        self.sock.send(header)
        self.sock.send(payload)
    
    def read_single_int(self):
        data = self.recv_exact(16)
        _, _, _, value = struct.unpack('!IIII', data)
        return value
    
    def write_single_string(self, client_id, op_id, string):
        b = string.encode('utf-8')
        str_size = len(b)
        msg_size = 12 + 4 + str_size
        header = struct.pack('!III', msg_size, client_id, op_id)
        len_payload = struct.pack('!I', str_size)
        self.sock.send(header)
        self.sock.send(len_payload)
        self.sock.send(b)
    
    def read_single_string(self):
        header = self.recv_exact(12)
        msg_size, client_id, op_id = struct.unpack('!III', header)
        len_data = self.recv_exact(4)
        str_len = struct.unpack('!I', len_data)[0]
        str_data = self.recv_exact(str_len)
        return str_data.decode('utf-8')
    
    def do_login(self):
        user = input("Utilizator: ").strip()
        password = input("Parola: ").strip()
        
        self.write_single_string(0, OPR_LOGIN, user)
        self.write_single_string(0, OPR_LOGIN, password)
        
        self.session_id = self.read_single_int()
        if self.session_id > 0:
            self.current_user = user
            print(f"Autentificare reusita! Session ID: {self.session_id}")
            return True
        else:
            print("Autentificare esuata!")
            return False
    
    def do_register(self):
        user = input("Nou utilizator: ").strip()
        password = input("Parola noua: ").strip()
        
        self.write_single_string(0, OPR_REGISTER, user)
        self.write_single_string(0, OPR_REGISTER, password)
        
        self.session_id = self.read_single_int()
        if self.session_id > 0:
            self.current_user = user
            print(f"Cont creat cu succes! Session ID: {self.session_id}")
            return True
        print("Eroare la creare cont!")
        return False
    
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
        
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, filename)
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(len(points)))
        
        bbox_str = bbox if bbox else ""
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, bbox_str)
        
        epsilon_str = str(epsilon) if epsilon > 0 else ""
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, epsilon_str)
        
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(show_segments))
        
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(dist_idx1))
        self.write_single_string(self.session_id, OPR_UPLOAD_GEO, str(dist_idx2))
        
        for lat, lon in points:
            coord_str = f"{lat},{lon}"
            self.write_single_string(self.session_id, OPR_UPLOAD_GEO, coord_str)
        
        total_distance_str = self.read_single_string()
        point_count_str = self.read_single_string()
        segment_count_str = self.read_single_string()
        
        total_distance = float(total_distance_str)
        point_count = int(point_count_str)
        segment_count = int(segment_count_str)
        
        segment_distances = []
        for i in range(segment_count):
            dist_str = self.read_single_string()
            segment_distances.append(float(dist_str))
        
        direct_distance_str = self.read_single_string()
        route_distance_str = self.read_single_string()
        has_distance_request_str = self.read_single_string()
        show_segments_resp_str = self.read_single_string()
        
        direct_distance = float(direct_distance_str) if direct_distance_str else 0.0
        route_distance = float(route_distance_str) if route_distance_str else 0.0
        has_distance_request = int(has_distance_request_str) if has_distance_request_str else 0
        show_segments_resp = int(show_segments_resp_str) if show_segments_resp_str else 0
        
        print(f"\n=== REZULTATE DE LA SERVER ===")
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
        
        return True

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
                self.write_single_int(self.session_id if self.session_id else 0, OPR_BYE, 0)
            except:
                pass
            self.sock.close()

    def print_usage(self):
        help_text = """
\033[1;33mComenzi disponibile:\033[0m
  upload <fisier>                                    - Upload simplu
  upload --bbox <min_lat,max_lat,min_lon,max_lon> <fisier>
  upload --simplify <epsilon> <fisier>               - Simplificare traseu
  upload --segments <fisier>                         - Afiseaza distante pe segmente
  upload --distance <idx1,idx2> <fisier>             - Distanta intre doua puncte
  <lat,lon> [lat,lon ...]                            - Introducere directa puncte
  help                                               - Acest mesaj
  exit                                               - Iesire

\033[1;33mExemple:\033[0m
  upload test.csv
  upload --bbox 44,48,20,30 test.csv
  upload --simplify 0.5 test.csv
  upload --segments test.csv
  upload --distance 1,5 test.csv
  44.4268,26.1025 45.7489,21.2087
"""
        print(help_text)

    def run_shell(self):
        if not self.connect():
            print("Nu se poate conecta la server!")
            return

        authenticated = False
        while not authenticated:
            print("\n\033[1;36m=== CLIENT GEOSPAȚIAL (Python) ===\033[0m")
            print("1. Autentificare")
            print("2. Creare cont nou")
            print("3. Iesire")
            choice = input("Alege: ").strip()
            
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

        while True:
            try:
                prompt = f"\n\033[1;32m{self.current_user}@geopython\033[0m> "
                cmd = input(prompt).strip()
                if not cmd:
                    continue
                if cmd == 'exit' or cmd == 'quit':
                    break
                elif cmd == 'help':
                    self.print_usage()
                    continue
                
                if cmd.startswith('upload '):
                    parts = cmd.split()
                    if len(parts) < 2:
                        print("Folosire: upload <fisier> [optiuni]")
                        continue
                    
                    bbox = None
                    epsilon = -1
                    show_segments = 0
                    dist_idx1 = 0
                    dist_idx2 = 0
                    filename = None
                    
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
                
                else:
                    points = self.parse_points_from_args(cmd.split())
                    if not points:
                        print("Comanda necunoscuta. Folositi 'help' pentru ajutor.")
                        continue
                    
                    self.send_points_to_server(points, "manual_input", None, -1, 0, 0, 0)
                    
            except KeyboardInterrupt:
                break
            except EOFError:
                break
            except (BrokenPipeError, ConnectionError) as e:
                print(f"\nConexiune pierduta cu serverul. Va rugam restartati clientul.\n")
                break

        self.close()
        print("\nClient inchis.")

def main():
    host = "127.0.0.1"
    port = 18081
    if len(sys.argv) > 1:
        host = sys.argv[1]
    if len(sys.argv) > 2:
        port = int(sys.argv[2])
    client = GeoClient(host, port)
    client.run_shell()

if __name__ == "__main__":
    main()
