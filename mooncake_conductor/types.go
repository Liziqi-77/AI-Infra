// Copyright 2025 AIBrix Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package kvevent

// ServiceType defines the type of service (vLLM or Mooncake)
type ServiceType string

const (
	ServiceTypeVLLM     ServiceType = "vLLM"
	ServiceTypeMooncake ServiceType = "Mooncake"
)

// ServiceConfig defines static connection information for a service instance.
// It replaces the dynamic Pod discovery mechanism from Kubernetes.
type ServiceConfig struct {
	Name      string      // Unique identifier (e.g., "vllm-worker-0")
	IP        string      // Service IP address
	Port      int         // ZMQ publisher port (e.g., 5557)
	Type      ServiceType // Service type (vLLM/Mooncake)
	ModelName string      // Model name hosted by the service
	LoraID    int64       // LoRA ID (-1 if not applicable)
}

